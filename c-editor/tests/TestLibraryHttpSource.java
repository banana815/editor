package org.notelab.editor;
import java.net.Authenticator;
import java.net.CookieHandler;
import java.net.CookieManager;
import java.nio.charset.StandardCharsets;
import java.io.ByteArrayOutputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Isolated JVM fixture for the exact Android transport source. */
public final class TestLibraryHttpSource {
    private static final String BODY="[{\"id\":\"local-test\",\"type\":\"book\",\"title\":\"中文🙂\"}]";
    private static final class Sink implements LibraryHttpSource.Sink {
        final CountDownLatch ended=new CountDownLatch(1);
        final ByteArrayOutputStream body=new ByteArrayOutputStream();
        final long deadline;
        volatile int state; // 0 pending, 1 done, 2 failed/canceled
        int doneCount;
        Sink(int timeout) { deadline=System.nanoTime()+TimeUnit.MILLISECONDS.toNanos(timeout); }
        public boolean active() { if(System.nanoTime()>=deadline) state=2;return state==0; }
        public boolean response(int status) { if(status<200||status>=300) state=2;return active(); }
        public boolean bytes(byte[] bytes,int size) { if(!active()) return false;body.write(bytes,0,size);return true; }
        public void success() { if(active()) state=1; }
        public void error(int reason) { if(state==0) state=2; }
        public void done() { doneCount++;ended.countDown(); }
        void waitDone() throws InterruptedException {
            if(!ended.await(5,TimeUnit.SECONDS)) throw new AssertionError("worker did not finish");
            if(doneCount!=1) throw new AssertionError("duplicate completion");
        }
    }
    private static Sink run(String url,int limit,int timeout) throws InterruptedException {
        Sink sink=new Sink(timeout);
        LibraryHttpSource task=new LibraryHttpSource(url,timeout,limit,sink);
        if(!task.start()) throw new AssertionError("cannot start");
        sink.waitDone();return sink;
    }
    private static void success(String base,String path,String expected) throws InterruptedException {
        Sink result=run(base+path,4096,3000);
        if(result.state!=1 || !new String(result.body.toByteArray(),StandardCharsets.UTF_8).equals(expected)) throw new AssertionError(path);
    }
    private static void failure(String base,String path,int cap,int timeout) throws InterruptedException {
        if(run(base+path,cap,timeout).state!=2) throw new AssertionError("expected failure: "+path);
    }
    public static void main(String[] args) throws Exception {
        if(args.length<1) throw new AssertionError("fixture URL missing");
        String base=args[0];
        success(base,"/ok",BODY);success(base,"/gzip",BODY);success(base,"/chunked",BODY);success(base,"/empty","");
        success(base,"/cookie",BODY);success(base,"/ok",BODY);
        failure(base,"/auth",4096,3000);failure(base,"/redirect",4096,3000);failure(base,"/missing",4096,3000);
        failure(base,"/truncated",4096,3000);failure(base,"/large",32,3000);failure(base,"/gzip",32,3000);failure(base,"/slow",4096,100);
        if(args.length>1) failure(args[1],"/ok",4096,3000);
        Sink canceled=new Sink(3000);LibraryHttpSource task=new LibraryHttpSource(base+"/drip",3000,4096,canceled);
        if(!task.start()) throw new AssertionError("start");Thread.sleep(30);canceled.state=2;
        long start=System.nanoTime();task.cancel();
        if(System.nanoTime()-start>TimeUnit.MILLISECONDS.toNanos(100)) throw new AssertionError("blocking cancel");
        canceled.waitDone();success(base,"/ok",BODY);
        CookieHandler.setDefault(new CookieManager());
        try { if(!LibraryHttpSource.hasGlobalHandlers()) throw new AssertionError("cookie guard");failure(base,"/ok",4096,3000); }
        finally { CookieHandler.setDefault(null); }
        Authenticator.setDefault(new Authenticator() {});
        try { if(!LibraryHttpSource.hasGlobalHandlers()) throw new AssertionError("auth guard on JDK");failure(base,"/ok",4096,3000); }
        finally { Authenticator.setDefault(null); }
        System.out.println("Android Java transport on JDK: UTF-8/gzip/chunks, TLS rejection, limits, status, cancellation and global-handler rejection passed");
    }
}
