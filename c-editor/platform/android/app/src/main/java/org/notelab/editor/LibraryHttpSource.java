package org.notelab.editor;

import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Method;
import java.net.Authenticator;
import java.net.CookieHandler;
import java.net.HttpURLConnection;
import java.net.Proxy;
import java.net.URL;
import java.util.zip.GZIPInputStream;

/** HTTP transport only. C validates URLs, limits, job state and library data. */
public final class LibraryHttpSource implements Runnable {
    interface Sink {
        boolean active();
        boolean response(int status);
        boolean bytes(byte[] data, int size);
        void success();
        void error(int reason);
        void done();
    }
    private static final class NativeSink implements Sink {
        private final long job;
        NativeSink(long job) { this.job = job; }
        public boolean active() { return nativeActive(job); }
        public boolean response(int status) { return nativeResponse(job, status); }
        public boolean bytes(byte[] bytes, int size) { return nativeBytes(job, bytes, size); }
        public void success() { nativeSuccess(job); }
        public void error(int reason) { nativeError(job, reason); }
        public void done() { nativeDone(job); }
    }
    private static native boolean nativeActive(long job);
    private static native boolean nativeResponse(long job, int status);
    private static native boolean nativeBytes(long job, byte[] bytes, int size);
    private static native void nativeSuccess(long job);
    private static native void nativeError(long job, int reason);
    private static native void nativeDone(long job);

    private final String url;
    private final int timeout;
    private final long limit;
    private final Sink sink;
    private volatile boolean canceled;
    private Thread worker;

    public static LibraryHttpSource create(String url, int timeout, long limit, long job) {
        return new LibraryHttpSource(url, timeout, limit, new NativeSink(job));
    }
    LibraryHttpSource(String url, int timeout, long limit, Sink sink) {
        this.url = url; this.timeout = timeout; this.limit = limit; this.sink = sink;
    }
    public boolean start() {
        try {
            worker = new Thread(this, "NoteLab-library-source");
            worker.setDaemon(true);
            worker.start();
            return true;
        } catch (RuntimeException | OutOfMemoryError failure) { return false; }
    }
    public void cancel() {
        canceled = true;
        Thread thread = worker;
        if (thread != null) thread.interrupt();
        // The blocking API owns its finite connect/read timeouts. Never make
        // the UI thread wait for disconnect while the worker is in native IO.
    }
    private boolean active() { return !canceled && sink.active(); }
    static boolean hasGlobalHandlers() {
        if (CookieHandler.getDefault() != null) return true;
        try {
            // Some Java runtimes expose this public getter. Android's current
            // API does not: its closed-app invariant is documented separately.
            Method getter = Authenticator.class.getMethod("getDefault");
            return getter.invoke(null) != null;
        } catch (NoSuchMethodException unavailable) {
            return false; // No private-field reflection or global mutation.
        } catch (ReflectiveOperationException | SecurityException denied) {
            return true; // A present getter that cannot be inspected fails closed.
        }
    }
    @Override public void run() {
        HttpURLConnection opened = null;
        try {
            if (!active()) return;
            if (hasGlobalHandlers()) { sink.error(4); return; }
            opened = (HttpURLConnection)new URL(url).openConnection(Proxy.NO_PROXY);
            opened.setInstanceFollowRedirects(false);
            opened.setRequestMethod("GET");
            opened.setConnectTimeout(timeout);
            opened.setReadTimeout(timeout);
            opened.setUseCaches(false);
            opened.setRequestProperty("Accept", "application/json, text/plain;q=0.9");
            opened.setRequestProperty("Accept-Encoding", "gzip");
            opened.setRequestProperty("User-Agent", "NoteLab/0.2");
            opened.setRequestProperty("Connection", "close");
            if (!active()) return;
            if (hasGlobalHandlers()) { sink.error(4); return; }
            int status = opened.getResponseCode();
            if (!active() || !sink.response(status)) return;
            String encoding = opened.getContentEncoding();
            boolean gzip = encoding != null && encoding.equalsIgnoreCase("gzip");
            if (encoding != null && !gzip && !encoding.equalsIgnoreCase("identity")) {
                sink.error(5); return;
            }
            long expected = opened.getContentLengthLong();
            try (InputStream raw = opened.getInputStream();
                 InputStream stream = gzip ? new GZIPInputStream(raw) : raw) {
                byte[] buffer = new byte[16384];
                long total = 0;
                while (active()) {
                    int count = stream.read(buffer);
                    if (!active()) return;
                    if (count == -1) {
                        if (!gzip && expected >= 0 && total != expected) sink.error(6);
                        else sink.success();
                        return;
                    }
                    if (count == 0) continue;
                    if (count > limit - total) { sink.error(1); return; }
                    if (!sink.bytes(buffer, count)) return;
                    total += count;
                }
            }
        } catch (OutOfMemoryError failure) {
            sink.error(3);
        } catch (IOException | RuntimeException failure) {
            sink.error(0); // Do not disclose URL queries or server response text.
        } finally {
            try { if (opened != null) opened.disconnect(); }
            finally { sink.done(); } // Last native job access, including cancellation.
        }
    }
}
