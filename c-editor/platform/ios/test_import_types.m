#import <Foundation/Foundation.h>
#include "import_types.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    @autoreleasepool {
        assert(PLATFORM_IMPORT_DOCUMENT == 3);
        NSArray<UTType *> *document = notelab_import_types(PLATFORM_IMPORT_DOCUMENT);
        for (NSString *extension in @[@"json", @"html", @"htm", @"md", @"txt"]) {
            UTType *candidate = [UTType typeWithFilenameExtension:extension];
            assert(candidate);
            BOOL selectable = NO;
            for (UTType *allowed in document) {
                if ([candidate conformsToType:allowed]) selectable = YES;
            }
            assert(selectable);
        }
        assert(![document containsObject:UTTypeData]);
        assert([notelab_import_types(PLATFORM_IMPORT_JSON) isEqualToArray:@[UTTypeJSON]]);
        assert([notelab_import_types(PLATFORM_IMPORT_SQLITE) isEqualToArray:@[UTTypeData]]);
        assert([notelab_import_types(PLATFORM_IMPORT_ZOTERO) isEqualToArray:@[UTTypeData]]);
    }
    puts("iOS import UTType filters passed");
    return 0;
}
