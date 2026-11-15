#include "platform_import.h"
#include "../platform/ios/import_types.h"
#include <string.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <objc/runtime.h>

#if !__has_feature(objc_arc)
#error "Compile platform_import_ios.m with -fobjc-arc"
#endif

/* The picker has a weak delegate. Its associated object keeps the delegate
 * alive until UIKit releases the picker; the delegate never retains it. */
static char import_delegate_key;

static void read_selected_file(PlatformImport *job, NSURL *url) {
    @autoreleasepool {
        NSString *name = url.lastPathComponent;
        NSData *name_utf8 = [name dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
        if (!url.isFileURL || !name_utf8.length ||
            memchr(name_utf8.bytes, 0, name_utf8.length)) {
            platform_import_complete_error(job, "无法读取所选文件的 UTF-8 文件名");
            return;
        }

        __block NSMutableData *contents = nil;
        __block NSString *failure = nil;
        BOOL scoped = [url startAccessingSecurityScopedResource];
        @try {
            NSError *coordination_error = nil;
            NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
            [coordinator coordinateReadingItemAtURL:url options:0 error:&coordination_error
                                        byAccessor:^(NSURL *coordinated_url) {
                NSNumber *regular = nil, *size = nil;
                NSError *error = nil;
                if (![coordinated_url getResourceValue:&regular forKey:NSURLIsRegularFileKey error:&error] ||
                    !regular.boolValue) {
                    failure = error.localizedDescription ?: @"请选择普通文件";
                    return;
                }
                if (![coordinated_url getResourceValue:&size forKey:NSURLFileSizeKey error:&error]) {
                    failure = error.localizedDescription ?: @"无法读取文件大小";
                    return;
                }
                if (size.unsignedLongLongValue > PLATFORM_IMPORT_MAX_BYTES) {
                    failure = @"文件超过 128 MiB，未导入";
                    return;
                }
                NSInputStream *stream = [NSInputStream inputStreamWithURL:coordinated_url];
                if (!stream) { failure = @"无法打开所选文件"; return; }
                contents = [[NSMutableData alloc] init];
                if (!contents) { failure = @"内存不足，未导入"; return; }
                [stream open];
                @try {
                    uint8_t buffer[64 * 1024];
                    for (;;) {
                        NSInteger count = [stream read:buffer maxLength:sizeof(buffer)];
                        if (count == 0) break;
                        if (count < 0) {
                            failure = stream.streamError.localizedDescription ?: @"读取文件失败";
                            break;
                        }
                        /* Recheck during the read: the provider may change the
                         * file after the size query, or report an unknown size. */
                        if (contents.length > PLATFORM_IMPORT_MAX_BYTES - (size_t)count) {
                            failure = @"文件超过 128 MiB，未导入";
                            break;
                        }
                        [contents appendBytes:buffer length:(NSUInteger)count];
                    }
                } @finally {
                    [stream close];
                }
            }];
            if (coordination_error) failure = coordination_error.localizedDescription;
        } @catch (NSException *exception) {
            (void)exception;
            failure = @"无法读取所选文件，未导入";
        } @finally {
            /* A false result also occurs for files already in our container;
             * attempt the coordinated read, and only balance successful starts. */
            if (scoped) [url stopAccessingSecurityScopedResource];
        }

        if (failure || !contents) {
            platform_import_complete_error(job, failure.UTF8String ?: "无法读取所选文件");
            return;
        }
        platform_import_complete_bytes(job, name.UTF8String, contents.bytes, contents.length);
        /* Completion publishes an owned copy. Do not touch job again. */
    }
}

@interface NoteLabImportDelegate : NSObject <UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate>
@property(nonatomic, assign) PlatformImport *job;
- (void)cancel;
@end

@implementation NoteLabImportDelegate
- (void)dealloc {
    /* Also finish if UIKit drops a picker that could not be presented. */
    if (_job) platform_import_complete_cancel(_job);
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller;
    PlatformImport *job = self.job;
    if (!job) return;
    self.job = NULL;
    if (urls.count != 1) {
        platform_import_complete_error(job, "请选择一个文件");
        return;
    }
    NSURL *url = urls.firstObject;
    /* File providers may download from iCloud during coordination. Keep that
     * work off the UI thread, with a new coordinator for this one operation. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        read_selected_file(job, url);
    });
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    (void)controller;
    [self cancel];
}
- (void)cancel {
    PlatformImport *job = self.job;
    if (!job) return;
    self.job = NULL;
    platform_import_complete_cancel(job);
}
- (void)presentationControllerDidDismiss:(UIPresentationController *)controller {
    (void)controller;
    [self cancel];
}
@end

void platform_import_ios_begin(SDL_Window *window, PlatformImport *job, PlatformImportKind kind) {
    if (!window) {
        platform_import_complete_error(job, "无法显示文件选择窗口");
        return;
    }
    UIWindow *native_window = (__bridge UIWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    if (!native_window) {
        platform_import_complete_error(job, "无法取得 iOS 窗口");
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *presenter = native_window.rootViewController;
        while (presenter.presentedViewController) presenter = presenter.presentedViewController;
        if (!presenter || !presenter.view.window || presenter.isBeingDismissed || presenter.isBeingPresented) {
            platform_import_complete_error(job, "当前无法显示文件选择器，请稍后重试");
            return;
        }
        NSArray<UTType *> *types = notelab_import_types(kind);
        UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
            initForOpeningContentTypes:types asCopy:NO];
        NoteLabImportDelegate *delegate = [[NoteLabImportDelegate alloc] init];
        if (!picker || !delegate) {
            platform_import_complete_error(job, "无法创建文件选择器");
            return;
        }
        delegate.job = job;
        picker.delegate = delegate;
        picker.allowsMultipleSelection = NO;
        picker.shouldShowFileExtensions = YES;
        objc_setAssociatedObject(picker, &import_delegate_key, delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [presenter presentViewController:picker animated:YES completion:nil];
        picker.presentationController.delegate = delegate;
    });
}
