#ifndef PLATFORM_EXPORT_H
#define PLATFORM_EXPORT_H
#include <SDL3/SDL.h>

typedef struct PlatformExport PlatformExport;
typedef enum {
    PLATFORM_EXPORT_PENDING, PLATFORM_EXPORT_DONE,
    PLATFORM_EXPORT_CANCELED, PLATFORM_EXPORT_FAILED
} PlatformExportStatus;

/* Copies the data before opening a native save dialog / browser download.
 * Call and poll on the main thread. Never free a pending export: native dialogs
 * may complete on another thread. Neither the document nor App is borrowed. */
PlatformExport *platform_export_json(SDL_Window *window, const char *filename,
                                     const char *json);
PlatformExportStatus platform_export_status(PlatformExport *job, const char **message);
void platform_export_free(PlatformExport *job);
#endif
