#include "platform_export.h"
#include <string.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

#if !__has_feature(objc_arc)
#error "Compile platform_export_ios.m with -fobjc-arc"
#endif

/* The C job owns the original snapshot until completion. This bridge owns a
 * separate NSData and only ever removes its unique private staging directory. */
void platform_export_ios_complete(PlatformExport *job, PlatformExportStatus status, const char *message);
static char export_delegate_key;

@interface NoteLabExportDelegate : NSObject <UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate>
@property(nonatomic, assign) PlatformExport *job;
@property(nonatomic, strong) NSURL *stagingDirectory;
- (void)finish:(PlatformExportStatus)status message:(const char *)message;
@end

@implementation NoteLabExportDelegate
- (void)dealloc {
    if (_stagingDirectory) [[NSFileManager defaultManager] removeItemAtURL:_stagingDirectory error:nil];
    if (_job) platform_export_ios_complete(_job, PLATFORM_EXPORT_CANCELED, "已取消导出");
}
- (void)finish:(PlatformExportStatus)status message:(const char *)message {
    PlatformExport *job = self.job;
    if (!job) return;
    self.job = NULL;
    if (self.stagingDirectory) {
        [[NSFileManager defaultManager] removeItemAtURL:self.stagingDirectory error:nil];
        self.stagingDirectory = nil;
    }
    platform_export_ios_complete(job, status, message);
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller;
    /* UIKit's exporting picker coordinates the copy to the chosen provider;
     * it calls this only after the export operation has completed. */
    if (urls.count == 1) [self finish:PLATFORM_EXPORT_DONE message:"文档已导出"];
    else [self finish:PLATFORM_EXPORT_FAILED message:"未能完成文件导出"];
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    (void)controller;
    [self finish:PLATFORM_EXPORT_CANCELED message:"已取消导出"];
}
- (void)presentationControllerDidDismiss:(UIPresentationController *)controller {
    (void)controller;
    [self finish:PLATFORM_EXPORT_CANCELED message:"已取消导出"];
}
@end

void platform_export_ios_begin(SDL_Window *window, PlatformExport *job,
                               const char *filename, const void *json,size_t size) {
    UIWindow *native_window = window ? (__bridge UIWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL) : nil;
    NSString *name = filename ? [NSString stringWithUTF8String:filename] : nil;
    if (!native_window || !name.length || ![name isEqualToString:name.lastPathComponent] ||
        [name isEqualToString:@"."] || [name isEqualToString:@".."] ||
        [name rangeOfString:@"\\"].location != NSNotFound || !json) {
        platform_export_ios_complete(job, PLATFORM_EXPORT_FAILED, "无法创建 iOS 导出文件");
        return;
    }
    NSData *snapshot = [NSData dataWithBytes:json length:size];
    if (!snapshot) {
        platform_export_ios_complete(job, PLATFORM_EXPORT_FAILED, "内存不足，未导出");
        return;
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            NSURL *temporary = [NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES];
            NSURL *directory = [temporary URLByAppendingPathComponent:
                [@"NoteLab-export-" stringByAppendingString:NSUUID.UUID.UUIDString] isDirectory:YES];
            NSURL *file = [directory URLByAppendingPathComponent:name isDirectory:NO];
            NSError *error = nil;
            if (![[NSFileManager defaultManager] createDirectoryAtURL:directory
                    withIntermediateDirectories:NO attributes:nil error:&error]) {
                platform_export_ios_complete(job, PLATFORM_EXPORT_FAILED,
                    error.localizedDescription.UTF8String ?: "无法创建导出目录");
                return;
            }
            if (![snapshot writeToURL:file options:NSDataWritingAtomic error:&error]) {
                [[NSFileManager defaultManager] removeItemAtURL:directory error:nil];
                platform_export_ios_complete(job, PLATFORM_EXPORT_FAILED,
                    error.localizedDescription.UTF8String ?: "无法创建导出文件");
                return;
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                UIViewController *presenter = native_window.rootViewController;
                while (presenter.presentedViewController) presenter = presenter.presentedViewController;
                if (!presenter || !presenter.view.window || presenter.isBeingDismissed || presenter.isBeingPresented) {
                    [[NSFileManager defaultManager] removeItemAtURL:directory error:nil];
                    platform_export_ios_complete(job, PLATFORM_EXPORT_FAILED, "当前无法显示导出窗口，请稍后重试");
                    return;
                }
                UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
                    initForExportingURLs:@[file] asCopy:YES];
                NoteLabExportDelegate *delegate = [[NoteLabExportDelegate alloc] init];
                if (!picker || !delegate) {
                    [[NSFileManager defaultManager] removeItemAtURL:directory error:nil];
                    platform_export_ios_complete(job, PLATFORM_EXPORT_FAILED, "无法创建导出窗口");
                    return;
                }
                delegate.job = job;
                delegate.stagingDirectory = directory;
                picker.delegate = delegate;
                picker.shouldShowFileExtensions = YES;
                objc_setAssociatedObject(picker, &export_delegate_key, delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                [presenter presentViewController:picker animated:YES completion:nil];
                picker.presentationController.delegate = delegate;
            });
        }
    });
}
