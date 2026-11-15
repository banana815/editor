#import <Foundation/Foundation.h>
#include "platform_library_source_internal.h"
#include <stdio.h>

@interface NoteLabLibraryRequest : NSObject <NSURLSessionDataDelegate>
@property(nonatomic,assign) PlatformLibrarySource *job; /* Delegate queue only after start. */
@property(atomic,strong) NSURLSession *session;
@property(atomic,strong) NSURLSessionDataTask *task;
@end
@implementation NoteLabLibraryRequest
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task
    didReceiveResponse:(NSURLResponse *)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completion {
    (void)session;(void)task;
    BOOL allowed=NO;
    if([response isKindOfClass:[NSHTTPURLResponse class]])
        allowed=platform_library_source_response(self.job,(unsigned)((NSHTTPURLResponse *)response).statusCode);
    else platform_library_source_error(self.job,"Library source did not return an HTTP response");
    completion(allowed?NSURLSessionResponseAllow:NSURLSessionResponseCancel);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    (void)session;
    if(!platform_library_source_append(self.job,data.bytes,data.length)) [task cancel];
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request
    completionHandler:(void (^)(NSURLRequest *))completion {
    (void)session;(void)task;(void)request;
    platform_library_source_response(self.job,(unsigned)response.statusCode);
    completion(nil); /* Do not contact a URL the user did not request. */
}
- (void)URLSession:(NSURLSession *)session didReceiveChallenge:(NSURLAuthenticationChallenge *)challenge
    completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition,NSURLCredential *))completion {
    (void)session;
    if([challenge.protectionSpace.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust])
        completion(NSURLSessionAuthChallengePerformDefaultHandling,nil);
    else completion(NSURLSessionAuthChallengeCancelAuthenticationChallenge,nil);
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
    didReceiveChallenge:(NSURLAuthenticationChallenge *)challenge
    completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition,NSURLCredential *))completion {
    (void)task;[self URLSession:session didReceiveChallenge:challenge completionHandler:completion];
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    (void)task;
    if(error) {
        char text[128];snprintf(text,sizeof text,"Library source network error (%ld)",(long)error.code);
        platform_library_source_error(self.job,text);
    } else platform_library_source_success(self.job);
    /* Unlike invalidateAndCancel, this waits for the final task/delegate calls
     * before didBecomeInvalid releases the backend's C reference. */
    [session finishTasksAndInvalidate];
}
- (void)URLSession:(NSURLSession *)session didBecomeInvalidWithError:(NSError *)error {
    (void)session;
    __attribute__((objc_precise_lifetime)) NoteLabLibraryRequest *keepAlive=self;
    PlatformLibrarySource *job=self.job;self.job=NULL;self.task=nil;self.session=nil;
    if(job) {
        if(error) platform_library_source_error(job,"Library source session was invalidated");
        platform_library_source_backend_done(job);
    }
    (void)keepAlive;
}
@end
static bool apple_start(PlatformLibrarySource *job,void *userdata) {
    (void)userdata;
    @autoreleasepool {
        NSString *text=[[NSString alloc] initWithUTF8String:platform_library_source_url(job)];
        NSURL *url=text?[NSURL URLWithString:text]:nil;
        NSURLSessionConfiguration *config=[NSURLSessionConfiguration ephemeralSessionConfiguration];
        NoteLabLibraryRequest *context=[NoteLabLibraryRequest new];
        NSOperationQueue *queue=[NSOperationQueue new];
        if(!url || !config || !context || !queue) return false;
        queue.maxConcurrentOperationCount=1;
        config.URLCache=nil;config.HTTPCookieStorage=nil;config.URLCredentialStorage=nil;
        config.HTTPShouldSetCookies=NO;config.requestCachePolicy=NSURLRequestReloadIgnoringLocalCacheData;
        config.connectionProxyDictionary=@{}; /* nil would select system proxy settings. */
        config.waitsForConnectivity=NO;
        NSTimeInterval seconds=(double)platform_library_source_timeout(job)/1000.0;
        config.timeoutIntervalForRequest=seconds;config.timeoutIntervalForResource=seconds;
        context.job=job;
        platform_library_source_set_context(job,(void *)CFBridgingRetain(context));
        @try {
            context.session=[NSURLSession sessionWithConfiguration:config delegate:context delegateQueue:queue];
            if(!context.session) return false;
            NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:seconds];
            request.HTTPMethod=@"GET";request.HTTPShouldHandleCookies=NO;
            [request setValue:@"application/json, text/plain;q=0.9" forHTTPHeaderField:@"Accept"];
            [request setValue:@"NoteLab/0.2" forHTTPHeaderField:@"User-Agent"];
            context.task=[context.session dataTaskWithRequest:request];
            if(!context.task) {
                platform_library_source_error(job,"Could not create library source request");
                [context.session finishTasksAndInvalidate];
            } else [context.task resume];
            return true;
        } @catch(NSException *exception) {
            (void)exception;
            if(!context.session) return false;
            platform_library_source_error(job,"Could not configure library source request");
            [context.task cancel];[context.session finishTasksAndInvalidate];return true;
        }
    }
}
static void apple_cancel(PlatformLibrarySource *job) {
    @autoreleasepool {
        NoteLabLibraryRequest *context=(__bridge NoteLabLibraryRequest *)platform_library_source_context(job);
        [context.task cancel];
    }
}
static void apple_destroy(PlatformLibrarySource *job) {
    void *pointer=platform_library_source_context(job);
    if(pointer) { @autoreleasepool { id context=CFBridgingRelease(pointer);(void)context; } }
}
const PlatformLibrarySourceTransport platform_library_source_transport={apple_start,apple_cancel,apple_destroy};
