#ifndef CE_WORKSPACE_BACKUP_H
#define CE_WORKSPACE_BACKUP_H
#include <stdbool.h>
#include <stddef.h>
/* The selected active workspace is captured byte-for-byte, including unknown
 * regular files and writing/library sidecars. Symlinks/reparse points fail;
 * recovery-control trees and known atomic-write temporary files are omitted.
 * ZIP is limited to 128 MiB / 10000 entries. Outputs untouched on failure. */
bool ce_workspace_backup(const char *root,unsigned char **bytes,size_t *size,char *error,size_t cap);
/* Fully validate before creating an exclusive fresh workspace. A failed write
 * may leave an unactivated partial staging folder; existing data is untouched.
 * Both returned strings use free(). The caller validates application state in
 * the staged root before explicitly activating its relative path. */
bool ce_workspace_restore_stage(const char *base,const void *bytes,size_t size,char **relative,char **root,char *error,size_t cap);
/* Atomic activation marker update; failure preserves the previous marker.
 * Empty relative selects the original workspace. Resolve fails closed on a
 * malformed marker or missing/symlink destination; it never changes storage. */
bool ce_workspace_activate(const char *base,const char *relative,char *error,size_t cap);
bool ce_workspace_resolve(const char *base,char **root,char *error,size_t cap);
#endif
