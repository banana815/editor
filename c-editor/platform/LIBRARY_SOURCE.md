# Explicit library sources

The app calls `platform_library_source_fetch` only after a user chooses Get or
Refresh. The transport does not discover Zotero, read configuration files or
credentials, parse CSL JSON, update the library, or persist a source. The C app
must parse and validate the complete result into a candidate library and commit
that candidate and its source settings together. An HTTP success alone is not a
valid library. Cancel, download failure and parse failure leave the current
library unchanged. Saving a URL is not permission to fetch it at startup.

Copy a current pull URL from Better BibTeX's export dialog instead of assuming a
fixed endpoint. Its CSL JSON response can have `text/plain` content type; the C
parser, rather than the MIME label, determines validity. `127.0.0.1` and
`localhost` always refer to the device running NoteLab, not another computer.

## C contract and limits

`include/platform_library_source.h` owns the public contract. All public job
operations run on the main thread; the C guard serializes worker callbacks.
Successful `take_result` transfers an owned byte buffer with an explicit byte
length and an additional zero sentinel. Binary NUL and invalid UTF-8 are not
silently transformed by the transport. The library parser must reject them.
`result_free` releases the transferred result independently of job lifetime.

Limits are 8,192 URL bytes, 128 MiB accumulated decoded body bytes, eight live
jobs, a default 30-second deadline and an absolute 120-second deadline. Callers
may lower the size cap. Input/output size arithmetic is checked in C. Internal
network/TLS implementation allocations are outside the decoded-body cap. Partial
bytes are discarded at failure/cancel. A canceled or freed job retains its
private backend reference until late OS callbacks have drained; it cannot update
an application object. Undrained canceled jobs still count toward the limit.

Cancellation and deadline expiry are immediately terminal when polled. The
application must continue polling pending jobs. Worker cleanup can outlive that
state: Android Java DNS and blocking socket calls are not forcibly killed, and
WinHTTP request handles are retained until their final close notification. There
is no unbounded retry. No application callback is invoked during free/cancel.

URLs must be ASCII HTTP(S), with non-ASCII path/query bytes percent-encoded.
Credentials, fragments, controls, backslashes, malformed escapes and ambiguous
numeric IPv4 spellings are rejected. Ports are 1–65535. HTTP permits only exact
`localhost`, `.localhost`/`.local` suffixes, canonical loopback/private/link-local
IP literals and mapped IPv6 equivalents. Public hosts require HTTPS. IPv6 zone
identifiers are explicitly unsupported. Redirects are never followed. Server
certificate/hostname verification is never disabled.

## Platform transports

| Platform | Transport | Configuration |
| --- | --- | --- |
| Windows | WinHTTP async API in a private worker | Direct connection; redirects, cookies and authentication disabled; system TLS |
| macOS/iOS | Private ephemeral NSURLSession | No cache/cookie/credential store or proxy dictionary; server-trust verification only; other auth challenges canceled |
| Linux | System libcurl >= 7.84 | Direct connection; no netrc/cookies/auth/redirects; async DNS and thread-safe initialization required |
| Android | HttpURLConnection worker + JNI byte callbacks | Direct connection; no redirects/cache; explicit gzip decoding; finite connect/read timeout; no global handler mutation |
| Browser | fetch + ReadableStream + AbortController | credentials omitted, no cache/referrer/redirect; CORS and browser local-network policy enforced |

Platform code only transfers status/bytes and manages OS lifecycle. Common C
code validates URLs and enforces terminal state, deadline and decoded-byte caps;
all metadata interpretation stays in C. Linux uses the installed TLS backend and
trust store, not a newly vendored OpenSSL build. Browser routing/proxy behavior
remains controlled by the browser; JavaScript cannot override that system policy.

Windows checks any remaining `Content-Length` against the accumulated body before
reporting success: WinHTTP can report zero-byte completion after a premature
connection close. Invalid or over-limit lengths fail before reading. WinHTTP
removes the length header when automatically decompressing, so the encoded size
is never compared with decoded bytes. Chunked and connection-delimited responses
without a length remain supported; every body still has the decoded-byte cap.

Android's public HttpURLConnection API lacks a per-request authenticator switch.
This implementation therefore depends on the closed application invariant that
neither NoteLab nor its SDL Java sources install a VM-wide `Authenticator`, alter
default TLS factories/verifiers, or install a cookie handler. A present cookie
handler is detected before construction and immediately before connection and
causes failure. On runtimes exposing public `Authenticator.getDefault`, a present
handler also causes failure. Current Android APIs lack that getter; no private
reflection or credential callback is attempted. This is not a transport for
arbitrary Java applications that install or concurrently change global handlers.
The source audit and isolated JVM tests are recorded below. Future Java network
features must preserve this invariant or replace this transport first.

Android currently targets API 35. INTERNET is declared. The manifest permits
cleartext because static per-domain XML cannot represent arbitrary user-entered
private addresses; the C URL guard restricts every request. Targeting API 37 will
require its local-network runtime permission flow before claiming support there.
Apple plists declare local-network access and narrow ATS exceptions for the same
local IP ranges/localhost, with no global arbitrary-load exception. OS permission
denial is a normal request failure and must not replace the library.

Better BibTeX documents that Zotero blocks browser pull exports. Browser CORS,
HTTPS mixed-content and local-network permission rules can impose additional
limits. This feature does not add a proxy, disable browser security or bypass
Zotero's policy. File import remains available when direct fetching is blocked.

## Desktop SQLite refresh

`platform_library_source_reopen_sqlite` accepts only an already user-selected,
absolute UTF-8 path to an existing regular file. It returns a copied native path,
not copied SQLite bytes, so the existing read-only Zotero loader still observes
SQLite WAL state. This check is not a persistent security grant or a snapshot of
the file; the loader must report changes/open failures safely. Mobile and browser
refresh by path explicitly fail and require choosing the file again. iOS security
scope bookmarks and Android persistable content-provider grants are not fabricated.

## Verification

`tests/test_library_source.c` links the common C implementation and its own mock
transport, not a platform backend. It covers URL boundaries, byte arithmetic,
terminal state, exact-once ownership, deadline/cancel/late replies, eight-job
admission and every reached module allocation failure with per-job hooks. It runs
with native ASan/UBSan and as actual WebAssembly; no global allocator is changed.

`tests/test_library_source_native_http.py <native-http-executable>` starts private
localhost HTTP/TLS fixtures for `test_library_source_native_http.c`. It tests
plain/gzip/chunked/connection-delimited bytes, redirects/auth/errors/truncated
bodies (including zero received bytes), lengths above 32 bits, decoded caps,
timeouts/cancel/free while pending, absent cookies and a self-signed certificate
that must be rejected. If its openssl test tool is unavailable, it explicitly
reports the TLS fixture as skipped. It never contacts the user's Zotero. The
native fixture runs on Windows, macOS and Linux in `.github/workflows/c-editor.yml`;
the current branch's results are available in [PR #3 checks](https://github.com/banana815/editor/pull/3/checks).
Local Apple/libcurl results do not substitute for actual Windows execution.

`TestLibraryHttpSource.java` compiles with the exact Android transport source and
runs the same fixture in an isolated JDK JVM, additionally exercising global
cookie/auth handler rejection and cancellation. This verifies Java transport
behavior, not Android device behavior. JNI has strict C11 syntax validation;
Android builds/runtime permission behavior still require Android CI/device tests.
Run this fixture with `python3 tests/test_library_source_android.py` and a JDK on
PATH; compilation/classes/certificates stay in temporary directories.
Apple transport compiles with the macOS, iOS device and iOS simulator SDKs; local
network permission/ATS behavior on iOS still requires device/simulator evidence.

The standalone WebAssembly harness uses `test_library_source_web.c` plus the
common C and web backend, SDL3, `--no-entry`, `-sMODULARIZE=1`,
`-sALLOW_MEMORY_GROWTH=1`, `-sEXPORTED_RUNTIME_METHODS=cwrap,UTF8ToString,HEAPU8`,
and web+node environments. The backend exports its own matched allocation pair;
it does not require the application to export libc malloc/free. Then run:

```
node tests/test_library_source_web.mjs <build>/test_library_source_web.cjs
node tests/test_library_source_browser.mjs <build>/test_library_source_web.cjs [chrome-path]
```

The first executes production EM_JS with deterministic Promise/stream failures in
actual WASM. The second uses actual Chromium with two local origins to verify
both allowed and blocked CORS, streaming decompression, cap/deadline/cancellation,
no cookies or redirect, and result bytes. Neither reads a user browser profile.

## Primary sources

- [Better BibTeX pull export](https://retorque.re/zotero-better-bibtex/exporting/pull/)
- [SDL_net API scope](https://wiki.libsdl.org/SDL3_net/CategoryAPIFunction)
- [curl removal of Secure Transport](https://curl.se/dev/deprecate.html)
- [WinHTTP concurrency](https://learn.microsoft.com/en-us/windows/win32/winhttp/concurrency-in-winhttp)
- [WinHTTP read completion](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpreaddata)
- [Microsoft .NET WinHTTP incomplete-body check](https://github.com/dotnet/runtime/blob/main/src/libraries/System.Net.Http.WinHttpHandler/src/System/Net/Http/WinHttpRequestCallback.cs)
- [Microsoft .NET WinHTTP response header handling](https://github.com/dotnet/runtime/blob/main/src/libraries/System.Net.Http.WinHttpHandler/src/System/Net/Http/WinHttpResponseParser.cs)
- [NSURLSession proxy configuration](https://developer.apple.com/documentation/foundation/urlsessionconfiguration/connectionproxydictionary)
- [Apple local networking and ATS IP exceptions](https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking)
- [Android HttpURLConnection cookie/auth handling](https://developer.android.com/reference/java/net/HttpURLConnection)
- [Android cleartext policy](https://developer.android.com/privacy-and-security/security-config)
- [Android local-network permission](https://developer.android.com/privacy-and-security/local-network-permission)
- [Chrome local-network access](https://developer.chrome.com/blog/local-network-access)
