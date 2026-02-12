// Include this file in fx_user.h

#pragma once

#include "tx_api.h"

#if SQLITE_THREADSAFE
#define FX_FILE_MODULE_EXTENSION unsigned open_count;         \
                                 int delete_on_close;         \
	                             unsigned shared_locks_count; \
 	                             int lock_type;               \
 	                             TX_THREAD* lock_task;        \
	                             TX_MUTEX mutex;
#else
#define FX_FILE_MODULE_EXTENSION unsigned open_count;         \
                                 int delete_on_close;         \
	                             unsigned shared_locks_count; \
 	                             int lock_type;               \
 	                             TX_THREAD* lock_task;
#endif
