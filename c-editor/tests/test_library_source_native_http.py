"""Local HTTP fixtures only. Never contacts Zotero or reads user configuration."""
import gzip
import http.server
import os
import socket
import socketserver
import ssl
import shutil
import tempfile
import subprocess
import sys
import threading
import time

BODY = '[{"id":"local-test","type":"book","title":"中文🙂"}]'.encode('utf-8')
violations = []

class LocalServer(http.server.ThreadingHTTPServer):
    def server_bind(self):
        # HTTPServer normally calls getfqdn even for 127.0.0.1. The fixture
        # must not depend on host DNS/reverse-DNS settings or their timeouts.
        socketserver.TCPServer.server_bind(self)
        self.server_name = 'localhost'
        self.server_port = self.server_address[1]

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def setup(self):
        self.request.settimeout(5)
        super().setup()
    def log_message(self, *_args):
        pass
    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError, TimeoutError, ssl.SSLError):
            pass
    def do_GET(self):
        if self.headers.get('Cookie') or self.headers.get('Authorization') or self.headers.get('Proxy-Authorization'):
            violations.append('unexpected credential or cookie')
        try:
            path = self.path
            if path == '/forbidden-redirect-target':
                violations.append('redirect followed')
            if path == '/slow':
                time.sleep(0.75)
            if path == '/redirect':
                self.send_response(302)
                self.send_header('Location', '/forbidden-redirect-target')
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            if path in ('/missing', '/auth'):
                self.send_response(401 if path == '/auth' else 404)
                if path == '/auth':
                    self.send_header('WWW-Authenticate', 'Basic realm=local-test')
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            body = b'' if path == '/empty' else BODY
            if path == '/large':
                body = b'x' * 256
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain; charset=utf-8')
            if path == '/cookie':
                self.send_header('Set-Cookie', 'must-not-return=secret')
            if path == '/gzip':
                body = gzip.compress(body)
                self.send_header('Content-Encoding', 'gzip')
            if path == '/chunked':
                self.send_header('Transfer-Encoding', 'chunked')
                self.end_headers()
                for at in range(0, len(body), 3):
                    block = body[at:at+3]
                    self.wfile.write(f'{len(block):x}\r\n'.encode() + block + b'\r\n')
                self.wfile.write(b'0\r\n\r\n')
                return
            self.send_header('Content-Length', str(len(body) + (100 if path == '/truncated' else 0)))
            self.end_headers()
            if path == '/drip':
                for byte in body:
                    self.wfile.write(bytes([byte]))
                    self.wfile.flush()
                    time.sleep(0.04)
            else:
                self.wfile.write(body)
                self.wfile.flush()
            if path == '/truncated':
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                self.close_connection = True
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass  # Intentional canceled/size-limited transfers.

server = LocalServer(('127.0.0.1', 0), Handler)
server.daemon_threads = True
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
tls_server = None
tls_thread = None
certificates = tempfile.TemporaryDirectory(prefix='notelab-http-cert-')
try:
    urls = [f'http://127.0.0.1:{server.server_port}']
    openssl = shutil.which('openssl')
    if openssl:
        certificate = os.path.join(certificates.name, 'test-cert.pem')
        key = os.path.join(certificates.name, 'test-key.pem')
        subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-config', os.devnull, '-keyout', key, '-out', certificate,
                        '-days', '1', '-subj', '/CN=localhost',
                        '-addext', 'subjectAltName=IP:127.0.0.1'], check=True,
                       timeout=15, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        tls_server = LocalServer(('127.0.0.1', 0), Handler)
        tls_server.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        # Handshake in a bounded request thread, not the accept loop: canceled
        # TLS clients must never prevent server.shutdown() from completing.
        tls_server.socket = context.wrap_socket(tls_server.socket, server_side=True,
                                                do_handshake_on_connect=False)
        tls_thread = threading.Thread(target=tls_server.serve_forever, daemon=True)
        tls_thread.start()
        urls.append(f'https://127.0.0.1:{tls_server.server_port}')
    else:
        print('TLS self-signed fixture skipped: openssl test tool is unavailable', flush=True)
    environment = os.environ.copy()
    # A broken explicit proxy environment must not redirect a user-selected
    # local library request. Do not inherit a proxy username/password.
    environment.update(http_proxy='http://127.0.0.1:1', https_proxy='http://127.0.0.1:1',
                       HTTP_PROXY='http://127.0.0.1:1', HTTPS_PROXY='http://127.0.0.1:1',
                       ALL_PROXY='http://127.0.0.1:1', all_proxy='http://127.0.0.1:1', NO_PROXY='', no_proxy='')
    # A command prefix also permits testing the exact Android Java transport in
    # an isolated JVM without a generated shell script or shell interpolation.
    if len(sys.argv) < 2:
        raise SystemExit('usage: test_library_source_native_http.py executable [arguments ...]')
    subprocess.run([*sys.argv[1:], *urls], check=True, timeout=30, env=environment)
    if tls_server:
        print('TLS self-signed certificate rejection passed', flush=True)
    assert not violations, violations
finally:
    if tls_server:
        tls_server.shutdown()
        tls_server.server_close()
        tls_thread.join()
    certificates.cleanup()
    server.shutdown()
    server.server_close()
    thread.join()
