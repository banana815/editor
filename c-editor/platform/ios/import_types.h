#ifndef NOTELAB_IOS_IMPORT_TYPES_H
#define NOTELAB_IOS_IMPORT_TYPES_H
#include "platform_import.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

/* Kept separate so the actual picker policy can be checked without UIKit. */
static inline NSArray<UTType *> *notelab_import_types(PlatformImportKind kind) {
    if (kind == PLATFORM_IMPORT_JSON) return @[UTTypeJSON];
    if (kind == PLATFORM_IMPORT_DOCUMENT) {
        NSMutableArray<UTType *> *types = [NSMutableArray arrayWithArray:
            @[UTTypeJSON, UTTypeHTML, UTTypePlainText]];
        /* Explicit extensions also cover provider-specific Markdown UTTypes.
         * Unknown tags can return nil; never put nil in a picker type array. */
        for (NSString *extension in @[@"json", @"html", @"htm", @"md", @"txt"]) {
            UTType *type = [UTType typeWithFilenameExtension:extension];
            if (type && ![types containsObject:type]) [types addObject:type];
        }
        return types;
    }
    /* SQLite providers do not consistently advertise a database UTType.
     * The C importer validates the selected bytes and SQLite signature. */
    return @[UTTypeData];
}
#endif
