#ifndef CE_APP_RECOVERY_STATE_H
#define CE_APP_RECOVERY_STATE_H
#include "platform_import.h"
typedef struct { PlatformImport *job; unsigned char *bytes; size_t size; bool busy; } RecoveryState;
#endif
