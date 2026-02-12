/*
 *   SQLite VFS for Eclipse ThreadX, former Azure RTOS
 *
 *   Created by Dmitry Rozhdestvenskiy, 2024,
 *
 *             embedded@mein.gmx
 *
 *          Designed in Switzerland
 *
 *
 *   Licensed like the SQLite itself, free for non-commercial and commercial use
 *   At the time the project was created I was unemployed so nobody may claim rights to this code
 *
 *   Tested with SQLite mptest multiwrite01.test
 *   As of December 2024 test passes with only four error in indexes (probably because of irregular collation)
 *   The only modification was changing randomblob(20000) to 10000
 *   and of course raising timeouts
 *
 *   There is also one bug in xOpen, sometimes crashes on creating semaphore (seems to be fixed)
 *
 *   Requires the following OS bugfix at least for OS 3.3.0 (invalidating cache for unaligned buffer leads to overrun-like behavior)
 *   https://community.st.com/t5/stm32-mcus-embedded-software/severe-error-in-azure-rtos-filex-corrupts-memory-solution/m-p/747323
 *   I did not find a way to make SQLite to strictly align buffers to a cache line
 *
 *   It is recommended to use with full C library as SQLite and debug code here uses 64-bit integers
 *   that are not supported for example in glibc-nano
 *
 *   !!! If you use more than one thread please pay attention to thread-safety of libc.
 *   On embedded systems it is often switched off by default
 *   And do not forget that Azure does not have file locking, so you may not use database files from processes
 *   other than that uses this VFS
 *
 */


// Main interface for memory management and mutexes
#include "sqlite3_azure.h"
#include "app_threadx.h"


#include "sqlite3.h"

#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <malloc.h>

/* ---------------------------- Configuration ---------------------------------------- */

// If defined, signals not to allocate large blocks. Sane for MCU
static const int SQLITE3_AZURE_CONFIG_SMALL_MALLOC = 1;

// Static main memory pool size
static const unsigned SQLITE3_AZURE_CONFIG_MEMORY_POOL_SIZE = sizeof(sqlite_heap); //0x48000;//

// Static separate page cache pool size
static const unsigned SQLITE3_AZURE_CONFIG_PAGE_POOL_SIZE = sizeof(sqlite_pcache);

// If static page cache pool memory is used
// SQLite must be compiled with SQLITE_ENABLE_MEMSYS3 or SQLITE_ENABLE_MEMSYS5

// If NULL will use heap memory as configured (main heap or Azure)
// If not NULL should than point to a static buffer 8 bytes aligned
// In the first test 128K was used
//#define SQLITE3_AZURE_CONFIG_STATIC_POOL (0x30000000) //sqlite_heap;
static void* const SQLITE3_AZURE_CONFIG_STATIC_POOL = sqlite_heap;

// If NULL will use default implementation
// If not NULL should than point to a static buffer 8 bytes aligned
// In the first test 64K was used
static void* const SQLITE3_AZURE_CONFIG_PAGE_POOL = sqlite_pcache;

// Sometimes it is better to align it to cache line
// Do not forget to align the buffer subsequently
#define SQLITE3_AZURE_CONFIG_PAGE_POOL_ALIGNMENT 32

// !!! Very important !!!
// If you try to work with an existing database that have larger page size
// the page cache (and the database itself) will very likely be corrupted !!!
// From the other hand, if you set this value to for example 4096 but will work with
// 512-byte pages you will waste most of page cache memory
// For me 512 was the only value that worked with small MCU memory
static const unsigned SQLITE3_AZURE_CONFIG_MAX_PAGE_SIZE = 512;

// This constant defines starting from what size the VFS will try to pre-allocate file space
static const ULONG64 PREALLOCATE_MINIMUM = 16536;

// If static pool used has no effect
// If NULL, sqlite will stay on standard malloc
// If not NULL should than be a _initialized_ TX_BYTE_POOL pointer
//#if !SQLITE3_AZURE_CONFIG_STATIC_POOL
//    #define SQLITE3_AZURE_CONFIG_DYNAMIC_POOL (NULL)
//#endif

// Additional scratch RAM region, may be NULL to discard
static void* const SQLITE3_AZURE_CONFIG_SCRATCH = NULL; // (void*)0x38000000;
static const unsigned SQLITE3_AZURE_CONFIG_SCRATCH_SIZE = 0x10000;

// Defines the number of structures that will keep malloc sizes
// Makes sense only together with SQLITE3_AZURE_CONFIG_DYNAMIC_POOL
#define SQLITE3_AZURE_CONFIG_TRACKABLE_MALLOCS 0

// Number of static mutexes SQLITE_MUTEX_STATIC_*
#define SQLITE3_AZURE_CONFIG_STATIC_MUTEXES (SQLITE_MUTEX_STATIC_VFS3 - SQLITE_MUTEX_RECURSIVE)

// Sets level of debug output
// 0 - no debug messages
// 1 - only xOpen, xClose, xRead and xWrite
// 2 - also xLock and xUnlock
// 3 - all messages
#define SQLITE3_AZURE_CONFIG_DEBUG 0

/* ---------------------------- Helper functions ------------------------------------- */

// Function that returns random number
static int (*sqlite3_xrandomness)(void) = rand; // TODO: thread safety

// If this function will be used the time will run from the default date you set in this function
static sqlite3_int64 azure_time64(void) { return tx_time_get() * 1000 / TX_TIMER_TICKS_PER_SECOND + 2460000 * 86400000ULL; }

static sqlite3_int64 (*sqlite3_time64)(void) = azure_time64;

static FX_MEDIA* sqlite3_media_ptr;

static UINT inline _mutex_create(TX_MUTEX *mutex_ptr, CHAR *name_ptr, UINT inherit)
{
#if SQLITE_THREADSAFE
    // Seems that this function is protected against preemption but not against interrupt
    tx_mutex_get(&(sqlite3_media_ptr -> fx_media_protect), TX_WAIT_FOREVER);
    UINT result = tx_mutex_create(mutex_ptr, name_ptr, inherit);
    tx_mutex_put(&(sqlite3_media_ptr -> fx_media_protect));

    return result;
#else
    return TX_SUCCESS;
#endif
}

static UINT inline _mutex_delete(TX_MUTEX *mutex_ptr)
{
#if SQLITE_THREADSAFE
    // Seems that this function is protected against preemption but not against interrupt
    tx_mutex_get(&(sqlite3_media_ptr -> fx_media_protect), TX_WAIT_FOREVER);
    UINT result = tx_mutex_delete(mutex_ptr);
    tx_mutex_put(&(sqlite3_media_ptr -> fx_media_protect));

    return result;
#else
    return TX_SUCCESS;
#endif
}

#if SQLITE_THREADSAFE
    #define mutex_get(a, b)  tx_mutex_get((a), (b))
    #define mutex_put(a)  tx_mutex_put((a))
    #define mutex_create(a, b, c)  _mutex_create((a), (b), (c))
    #define mutex_delete(a)  _mutex_delete((a))
#else
    #define mutex_get(a, b)
    #define mutex_put(a)
    #define mutex_create(a, b, c)
    #define mutex_delete(a)
#endif

#if SQLITE3_AZURE_CONFIG_DEBUG > 0
    static const char newline[] = "\n";
#endif

/* ---------------------------- File system ------------------------------------------ */

static int TranslateReturnValue(UINT value)
{
    switch(value)
    {
        case /*FX_SUCCESS           */ 0x00:
            return SQLITE_OK;
        case /* FX_BOOT_ERROR       */ 0x01:
        case /* FX_MEDIA_INVALID    */ 0x02:
        case /* FX_FAT_READ_ERROR   */ 0x03:
            return SQLITE_ERROR;
        case /* FX_NOT_FOUND        */ 0x04:
            return SQLITE_NOTFOUND;
        case /* FX_NOT_A_FILE       */ 0x05:
        case /* FX_ACCESS_ERROR     */ 0x06:
             return SQLITE_IOERR;
        case /* FX_NOT_OPEN         */ 0x07:
            return SQLITE_CANTOPEN;
        case /* FX_FILE_CORRUPT     */ 0x08:
            return SQLITE_CORRUPT;
        case /* FX_END_OF_FILE      */ 0x09:
            return SQLITE_IOERR_SHORT_READ;
        case /* FX_NO_MORE_SPACE    */ 0x0A:
        case /* FX_ALREADY_CREATED  */ 0x0B:
        case /* FX_INVALID_NAME     */ 0x0C:
        case /* FX_INVALID_PATH     */ 0x0D:
        case /* FX_NOT_DIRECTORY    */ 0x0E:
        case /* FX_NO_MORE_ENTRIES  */ 0x0F:
        case /* FX_DIR_NOT_EMPTY    */ 0x10:
        case /* FX_MEDIA_NOT_OPEN   */ 0x11:
        case /* FX_INVALID_YEAR     */ 0x12:
        case /* FX_INVALID_MONTH    */ 0x13:
        case /* FX_INVALID_DAY      */ 0x14:
        case /* FX_INVALID_HOUR     */ 0x15:
        case /* FX_INVALID_MINUTE   */ 0x16:
        case /* FX_INVALID_SECOND   */ 0x17:
        case /* FX_PTR_ERROR        */ 0x18:
        case /* FX_INVALID_ATTR     */ 0x19:
        case /* FX_CALLER_ERROR     */ 0x20:
            return SQLITE_ERROR;
        case /* FX_BUFFER_ERROR     */ 0x21:
            return SQLITE_IOERR;
        case /* FX_NOT_IMPLEMENTED  */ 0x22:
        case /* FX_WRITE_PROTECT    */ 0x23:
        case /* FX_INVALID_OPTION   */ 0x24:
        case /* FX_SECTOR_INVALID   */ 0x89:
            return SQLITE_ERROR;
        case /* FX_IO_ERROR         */ 0x90:
            return SQLITE_IOERR;
        case /* FX_NOT_ENOUGH_MEMORY*/ 0x91:
            return SQLITE_NOMEM;
        case /* FX_ERROR_FIXED      */ 0x92:
        case /* FX_ERROR_NOT_FIXED  */ 0x93:
        case /* FX_NOT_AVAILABLE    */ 0x94:
        case /* FX_INVALID_CHECKSUM */ 0x95:
        case /* FX_READ_CONTINUE    */ 0x96:
        case /* FX_INVALID_STATE    */ 0x97:
        default:
            return SQLITE_ERROR;
    }
}

struct sqlite3_azure_file
{
    // Inherited part
    const struct sqlite3_io_methods *pMethods;

    // Azure File management
    // Do not forget to include sqlite3_azure_fx_user.h in fx_user.h
    FX_FILE* fx_file;
};

const sqlite3_io_methods azure_file_methods;
sqlite3_vfs azure_vfs;

static TX_MUTEX openclose;

int xOpen(sqlite3_vfs* vfs, sqlite3_filename zName, sqlite3_file* fptr, int flags, int *pOutFlags)
{
    assert(vfs == &azure_vfs);
    assert(fptr);

    // Needed to generate unique temporary file names
    static unsigned temp_counter = 0;
    static char tempname[25];
    static const char tempname_fixed[] = "~sqlite3_temp-";
    unsigned tempname_fixed_length = strlen(tempname_fixed);

    struct sqlite3_azure_file* azure_fptr = (struct sqlite3_azure_file*)fptr;

    fptr->pMethods = &azure_file_methods;

    mutex_get(&openclose, TX_WAIT_FOREVER);

    if(NULL == zName)
    {   // If zName is NULL we must invent unique name (used for temporary files)
        strcpy(tempname, tempname_fixed);
        for(unsigned i = 1000000000; i > 0; i /= 10)
            tempname[tempname_fixed_length++] = (temp_counter / i) % 10 + '0';
        tempname[tempname_fixed_length] = 0;
        temp_counter++;
    }
    else
    {
        FX_FILE* look_fptr = sqlite3_media_ptr->fx_media_opened_file_list;

        // SQLite may open the same file for write several times, it relies on locking
        // Azure RTOS cannot do this so here is a workaround
        if(look_fptr)
        {
            for(unsigned i = 0; i < sqlite3_media_ptr->fx_media_opened_file_count; i++)
            {
                if(!strcmp(zName, look_fptr->fx_file_name)) // This file is already opened
                {
                    azure_fptr->fx_file = look_fptr; // We just save the pointer to the file structure already open
                    mutex_put(&openclose);
                    mutex_get(&look_fptr->mutex, TX_WAIT_FOREVER);
#if SQLITE3_AZURE_CONFIG_DEBUG > 0
    printf("xOpen reopens %s, ", zName);
#endif
                    look_fptr->delete_on_close |= flags & SQLITE_OPEN_DELETEONCLOSE;
                    look_fptr->open_count++;
                    UINT result = SQLITE_OK;
                    if((look_fptr->fx_file_open_mode == FX_OPEN_FOR_READ) && (flags & SQLITE_OPEN_READWRITE)) // Reopen for write
                    {   // If it is opened for read we may need to reopen
                        fx_file_close(look_fptr);
                        result = fx_file_open(sqlite3_media_ptr, look_fptr, (char*)zName, FX_OPEN_FOR_WRITE);
                        if(result == FX_ACCESS_ERROR)
                        {
                        	if(pOutFlags)
                        	{
                                *pOutFlags &= ~SQLITE_OPEN_READWRITE;
                                *pOutFlags |= SQLITE_OPEN_READONLY;
                        	}
                            result = fx_file_open(sqlite3_media_ptr, look_fptr, (char*)zName, FX_OPEN_FOR_READ);
                        }
                    }
#if SQLITE3_AZURE_CONFIG_DEBUG > 0
    printf("exit code %i%s", result, newline);
#endif
                    mutex_put(&look_fptr->mutex);

                    return result;
                }
                look_fptr = look_fptr->fx_file_opened_next;
                assert(fptr);
            }
        }
    }

    const char* name = zName ? zName : tempname;

#if SQLITE3_AZURE_CONFIG_DEBUG > 0
    printf("xOpen %s, ", name);
#endif

    if(pOutFlags)
        *pOutFlags = flags;
    UINT create_result;

    if(zName == NULL) // Delete temporary file if exists
        fx_file_delete(sqlite3_media_ptr, (char*)name);

    if(flags & SQLITE_OPEN_CREATE)
    {
        create_result = fx_file_create(sqlite3_media_ptr, (char*)name);

        if(FX_ALREADY_CREATED == create_result)
        {
            if(flags & SQLITE_OPEN_EXCLUSIVE)
            {
            	mutex_put(&openclose);
            	return SQLITE_CANTOPEN;
            }
        }
        else
            if(create_result != FX_SUCCESS)
            {
            	mutex_put(&openclose);
                return SQLITE_CANTOPEN;
            }
    }

    FX_FILE* fx_fptr = azure_fptr->fx_file = sqlite3_malloc(sizeof(FX_FILE));

    if(NULL == fx_fptr)
    {
    	mutex_put(&openclose);
    	return SQLITE_NOMEM;
    }

    fx_fptr->delete_on_close = flags & SQLITE_OPEN_DELETEONCLOSE;

    UINT open_type = (flags & SQLITE_OPEN_READWRITE) ? FX_OPEN_FOR_WRITE : FX_OPEN_FOR_READ;

    UINT result = fx_file_open(sqlite3_media_ptr, fx_fptr, (char*)name, open_type);
#if SQLITE3_AZURE_CONFIG_DEBUG > 0
    printf("exit code %i%s", result, newline);
#endif
    if((result == FX_ACCESS_ERROR) && (open_type & FX_OPEN_FOR_WRITE))
    {   // If the file is read-only try to open that at least so
        open_type = FX_OPEN_FOR_READ;
        if(pOutFlags)
        {
            *pOutFlags &= ~SQLITE_OPEN_READWRITE;
            *pOutFlags |= SQLITE_OPEN_READONLY;
        }
        result = fx_file_open(sqlite3_media_ptr, fx_fptr, (char*)name, open_type);
    }

    if(result != FX_SUCCESS)
    {
        //SQLite calls xClose despite, so we must signal that the file was not opened
        sqlite3_free(fx_fptr);
        azure_fptr->fx_file = NULL;
    }
    else
    {
        // Temporary files and journals must be used by the same process only, so locked immediately
        // Not tested with WAL journals
        if((flags & SQLITE_OPEN_MAIN_JOURNAL) || (NULL == zName))
        {
            fx_fptr->lock_type = SQLITE_LOCK_EXCLUSIVE;
            fx_fptr->lock_task = tx_thread_identify();
        }
        else
        {
            fx_fptr->lock_type = SQLITE_LOCK_NONE;
            fx_fptr->lock_task = NULL;
        }
        fx_fptr->shared_locks_count = 0;
        fx_fptr->open_count = 1;

        mutex_create(&(fx_fptr->mutex), "Azure file mutex", TX_NO_INHERIT);

        // Not good idea while if the file exists and is corrupt we receive an error
        // Just delete instead
        //if(zName == NULL) // If the temporary file persist destroy it (it is safe, rollbacks are in journals)
            //fx_file_truncate(fx_fptr, 0);
    }

	mutex_put(&openclose);

    return TranslateReturnValue(result);
}

int xDelete(sqlite3_vfs* vfs, const char *zName, int)
{
    assert(vfs == &azure_vfs);
    assert(zName);

    const UINT result = fx_file_delete(sqlite3_media_ptr, (char*)zName);

#if SQLITE3_AZURE_CONFIG_DEBUG > 2
    printf("xDelete %s, exit code %i%s", zName, result, newline);
#endif

    if(FX_SUCCESS == result)
        return SQLITE_OK;

    return SQLITE_IOERR_DELETE;
}

int xAccess(sqlite3_vfs* vfs, const char *zName, int flags, int *pResOut)
{
    assert(vfs == &azure_vfs);
    assert(zName);
    assert(pResOut);

#if SQLITE3_AZURE_CONFIG_DEBUG > 2
    printf("xAccess %s, flags %i%s", zName, flags, newline);
#endif

    UINT attributes;

    switch(flags)
    {
        case SQLITE_ACCESS_EXISTS:
            if(FX_SUCCESS == fx_file_attributes_read(sqlite3_media_ptr, (char*)zName, &attributes))
                *pResOut = -1;
            else
                *pResOut = 0;
            break;
        case SQLITE_ACCESS_READWRITE:
            if(FX_SUCCESS != fx_file_attributes_read(sqlite3_media_ptr, (char*)zName, &attributes))
               return SQLITE_NOTFOUND;
            else
               if(attributes & FX_READ_ONLY)
                  *pResOut = 0;
               else
                  *pResOut = -1;
            break;
        default:
            *pResOut = 0;
            break;
    }
    return SQLITE_OK;
}

int xFullPathname(sqlite3_vfs* vfs, const char *zName, int nOut, char *zOut)
{
    assert(vfs == &azure_vfs);
    assert(zName);
    assert(zOut);

#if SQLITE3_AZURE_CONFIG_DEBUG > 2
    printf("xFullPathName %s, nOut %i%s", zName, nOut, newline);
#endif

    UINT result = fx_directory_local_path_get_copy(sqlite3_media_ptr, zOut, nOut);
    if(FX_SUCCESS != result)
        return TranslateReturnValue(result);

    char* ptr = zOut;

    while(*ptr)
        ptr++;

    while((*(ptr++) = *(zName++)))
        if((ptr - zOut) >= nOut)
            return SQLITE_IOERR;

    return SQLITE_OK;
}

// Extensions not supported, just copied from demo VFS
// I have no idea may we just use NULL pointers for these functions or not
static void *xDlOpen(sqlite3_vfs* , const char *)
{
    return NULL;
}

static void xDlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg)
{
    sqlite3_snprintf(nByte, zErrMsg, "Loadable extensions are not supported");
    zErrMsg[nByte-1] = '\0';
}

static void (*xDlSym(sqlite3_vfs *pVfs, void *pH, const char *z))(void)
{
    return NULL;
}

static void xDlClose(sqlite3_vfs *pVfs, void *pHandle)
{
    return;
}

// It is sane to redefine this function if the platform has real random number generator
int xRandomness(sqlite3_vfs* vfs, int nByte, char *zOut)
{
    assert(vfs == &azure_vfs);
    assert(zOut);

    for(unsigned i = 0; i < nByte; i++)
    	zOut[i] = sqlite3_xrandomness() & 0xFF;

    return nByte;
}

int xSleep(sqlite3_vfs* vfs, int microseconds)
{
    assert(vfs == &azure_vfs);

    ULONG time_before = tx_time_get();

    tx_thread_sleep(microseconds * TX_TIMER_TICKS_PER_SECOND / 1000000U );

    ULONG time_after = tx_time_get();

    return (time_after - time_before) * 1000000U / TX_TIMER_TICKS_PER_SECOND;
}

int xCurrentTime(sqlite3_vfs* vfs, double* time)
{
    assert(vfs == &azure_vfs);
    assert(sqlite3_time64);
    assert(time);

    *time = (*sqlite3_time64)() / 86400000.0;
    return SQLITE_OK;
}

int xGetLastError(sqlite3_vfs* vfs, int, char *result) // TODO it is always called as (vfs, 0, 0), strange
{
    assert(vfs == &azure_vfs);

    if(result)
       *result = 0;

    return 0;
}

int xCurrentTimeInt64(sqlite3_vfs* vfs, sqlite3_int64* result)
{
    assert(vfs == &azure_vfs);
    assert(sqlite3_time64);
    assert(result);

    *result = (*sqlite3_time64)();
    return SQLITE_OK;
}

sqlite3_vfs azure_vfs = {
          .iVersion      = 2,                                 /* Structure version number (currently 3) */
          .szOsFile      = sizeof(struct sqlite3_azure_file), /* Size of subclassed sqlite3_file */
          .mxPathname    = FX_MAXIMUM_PATH,                   /* Maximum file pathname length */
          .pNext         = NULL,                              /* Next registered VFS */
          .zName         = "\"Microsoft Azure VFS\"",         /* Name of this virtual file system */
          .pAppData      = NULL,                              /* Pointer to application-specific data */
          .xOpen         = xOpen,
          .xDelete       = xDelete,
          .xAccess       = xAccess,
          .xFullPathname = xFullPathname,
          .xDlOpen       = xDlOpen,
          .xDlError      = xDlError,
          .xDlSym        = xDlSym,
          .xDlClose      = xDlClose,
          .xRandomness   = xRandomness,
          .xSleep        = xSleep,
          .xCurrentTime  = xCurrentTime,
          .xGetLastError = xGetLastError,
          /*
          ** The methods above are in version 1 of the sqlite_vfs object
          ** definition.  Those that follow are added in version 2 or later
          */
          .xCurrentTimeInt64 = xCurrentTimeInt64,
          /*
          ** The methods above are in versions 1 and 2 of the sqlite_vfs object.
          ** Those below are for version 3 and greater.
          */
          //.xSetSystemCall = xSetSystemCall,
          //.xGetSystemCall = xGetSystemCall,
          //.xNextSystemCall = xNextSystemCall
          /*
          ** The methods above are in versions 1 through 3 of the sqlite_vfs object.
          ** New fields may be appended in future versions.  The iVersion
          ** value will increment whenever this happens.
          */
};

static inline FX_FILE* convert_fptr(sqlite3_file* fptr)
{
	assert(fptr);

    FX_FILE* const azure_fptr = ((struct sqlite3_azure_file*)fptr)->fx_file;

    assert(azure_fptr);

    return azure_fptr;
}

int xClose(sqlite3_file* fptr)
{
    FX_FILE* const azure_fptr = convert_fptr(fptr);

    /*if(NULL == azure_fptr)
        return SQLITE_OK;*/

    mutex_get(&(azure_fptr->mutex), TX_WAIT_FOREVER);

    if(--(azure_fptr->open_count))
    {
        mutex_put(&(azure_fptr->mutex));
        return SQLITE_OK;
    }

#if SQLITE3_AZURE_CONFIG_DEBUG > 0
    printf("xClose %s%s", azure_fptr->fx_file_name, newline);
#endif

    FX_MEDIA* mediaptr = azure_fptr->fx_file_media_ptr;

    mutex_get(&openclose, TX_WAIT_FOREVER);
    mutex_put(&(azure_fptr->mutex));

    if(FX_SUCCESS == fx_file_close(azure_fptr))
    {
        if(((struct sqlite3_azure_file*)fptr)->fx_file->delete_on_close)
            fx_file_delete(mediaptr, azure_fptr->fx_file_name);

        mutex_delete(&(azure_fptr->mutex));

        sqlite3_free(azure_fptr);

#if SQLITE3_AZURE_CONFIG_DEBUG
        fx_media_flush(mediaptr); // In debug mode project crashes often, so it is not bad to flush
#endif

        mutex_put(&openclose);

        return SQLITE_OK;
    }

    mutex_put(&openclose);

    return SQLITE_IOERR_CLOSE;
}

/*
 *   As of version 3.3.0 Azure RTOS has a bug when data cache is used and the buffer is not aligned to cache line
 *   https://community.st.com/t5/stm32-mcus-embedded-software/severe-error-in-azure-rtos-filex-corrupts-memory-solution/m-p/747323
 *
 *   It could also be fixed here by double-buffering, but it would kill performance
 *   I hope that Azure team will implement my workaround
 */
int xRead(sqlite3_file* fptr, void* buffer, int iAmt, sqlite3_int64 iOfst)
{
	assert(buffer);
	assert(iAmt >= 0);
	assert(iOfst >= 0);

    FX_FILE* const azure_fptr = convert_fptr(fptr);

    // SQLite tends to read start of the database even if it is locked, seems there is a read-only header
    //assert((azure_fptr->lock_type < SQLITE_LOCK_EXCLUSIVE) || (azure_fptr->lock_task == tx_thread_identify()));

    ULONG actual_size = 0;
    ULONG retval = SQLITE_OK;

    // It assures that there is no attempt to obtain lock
    //    or seek it at the same time
    mutex_get(&(azure_fptr->mutex), TX_WAIT_FOREVER);

    // TODO Trying to read behind the EOF is error, not short read?
    if(azure_fptr->fx_file_current_file_size < iOfst)
        retval = SQLITE_IOERR_SHORT_READ; //SQLITE_IOERR_READ;
    else
       if(fx_file_extended_seek(azure_fptr, iOfst) != FX_SUCCESS)
           retval = SQLITE_IOERR_SEEK; //SQLITE_IOERR_READ;

    if(retval == SQLITE_OK)
        retval = TranslateReturnValue(fx_file_read(azure_fptr, buffer, iAmt, &actual_size));

#if SQLITE3_AZURE_CONFIG_DEBUG > 0
    printf("xRead %s %i bytes at %lld process %li, read %li%s", azure_fptr->fx_file_name, iAmt, iOfst,
            (ULONG)tx_thread_identify(), actual_size, newline);
#endif

    mutex_put(&(azure_fptr->mutex));

    if(actual_size < iAmt)
    {
        memset(((char*)buffer) + actual_size, 0, iAmt - actual_size);
        if(retval != SQLITE_IOERR_SEEK)
            retval = SQLITE_IOERR_SHORT_READ;
    }

    return retval;
}

int xWrite(sqlite3_file* fptr, const void* buffer, int iAmt, sqlite3_int64 iOfst)
{
    static const char _Alignas(SQLITE3_AZURE_CONFIG_PAGE_POOL_ALIGNMENT) zero_buffer[512] = {0}; // May adjust size, it is guarded

    FX_FILE* const azure_fptr = convert_fptr(fptr);

    assert(buffer);
	assert(iAmt >= 0);
	assert(iOfst >= 0);
    assert(azure_fptr->lock_type == SQLITE_LOCK_EXCLUSIVE);
    assert(azure_fptr->lock_task == tx_thread_identify());

    int retval = SQLITE_OK;

    // Seems that SQLite may sometimes read read-only parts of the database file (like header)
    // without acquiring SHARED lock. It would be safe for normal OS, but here we need to
    // atomize seek and write. Because of this write is also isolated
    mutex_get(&(azure_fptr->mutex), TX_WAIT_FOREVER);

    // Azure does not check this condition
    // Just sets to file size without error
    if(azure_fptr->fx_file_current_file_size < iOfst)
    {
        if(fx_file_extended_seek(azure_fptr, azure_fptr->fx_file_current_file_size) != FX_SUCCESS)
            retval = SQLITE_IOERR_SEEK;//SQLITE_IOERR_WRITE;
        else
        {
            while(azure_fptr->fx_file_current_file_size < iOfst)
            {
                ULONG size = iOfst - azure_fptr->fx_file_current_file_size;
                if(size > sizeof(zero_buffer))
                    size = sizeof(zero_buffer);
                if(fx_file_write(azure_fptr, (char*)zero_buffer, size) != FX_SUCCESS)
                {
                    retval = SQLITE_IOERR_WRITE;
                    break;
                }
            }
        }
    }
    else
        if(fx_file_extended_seek(azure_fptr, iOfst) != FX_SUCCESS)
            retval = SQLITE_IOERR_SEEK; //SQLITE_IOERR_WRITE;

    if(retval == SQLITE_OK)
        retval = TranslateReturnValue(fx_file_write(azure_fptr, (void*)buffer, iAmt));

#if SQLITE3_AZURE_CONFIG_DEBUG > 0
    printf("xWrite %s %i bytes at %lld process %li, memory used %i, %s%s", azure_fptr->fx_file_name, iAmt, iOfst,
            (ULONG)tx_thread_identify(), (int)sqlite3_memory_used(), retval == SQLITE_OK ? "SUCCESS" : "FAIL", newline);
#endif

    mutex_put(&(azure_fptr->mutex));

    return retval;
}

int xTruncate(sqlite3_file* fptr, sqlite3_int64 size)
{
    FX_FILE* const azure_fptr = convert_fptr(fptr);

#if SQLITE3_AZURE_CONFIG_DEBUG > 0
    printf("xTruncate %s to %lli bytes\r\n", azure_fptr->fx_file_name, size);
#endif

    assert(azure_fptr->lock_type == SQLITE_LOCK_EXCLUSIVE);
    assert(azure_fptr->lock_task == tx_thread_identify());

    int retval = SQLITE_OK;

    mutex_get(&(azure_fptr->mutex), TX_WAIT_FOREVER);

    if(fx_file_extended_truncate_release(azure_fptr, size) != FX_SUCCESS)
        retval = SQLITE_IOERR_TRUNCATE;

    mutex_put(&(azure_fptr->mutex));

    return retval;
}

int xSync(sqlite3_file* fptr, int flags)
{
    FX_FILE* const azure_fptr = convert_fptr(fptr);

#if SQLITE3_AZURE_CONFIG_DEBUG > 2
    printf("xSync%s", newline);
#endif

    // In fact we does not need it while SQLite uses it to synchronize after writing.
    // As this RTOS does not have separate buffers for individual files all writes are immediately visible for all
    // But it is still good to flush for safety
    return TranslateReturnValue(fx_media_flush(azure_fptr->fx_file_media_ptr));
}

int xFileSize(sqlite3_file* fptr, sqlite3_int64 *pSize)
{
    assert(pSize);

	FX_FILE* const azure_fptr = convert_fptr(fptr);

    *pSize = azure_fptr->fx_file_current_file_size;

#if SQLITE3_AZURE_CONFIG_DEBUG > 2
    printf("xFileSize %s, got %lld bytes%s", azure_fptr->fx_file_name, *pSize, newline);
#endif

    return SQLITE_OK;
}

int xLock(sqlite3_file* fptr, int lock)
{
    assert(lock != SQLITE_LOCK_NONE); // xLock may only upgrade lock

	FX_FILE* const azure_fptr = convert_fptr(fptr);

    int retval = SQLITE_OK;

    mutex_get(&(azure_fptr->mutex), TX_WAIT_FOREVER);

    /*
     * Prepare to hell
     * I really hate lack of the documentation about this
     *  Have spent most time here
     */

    // TODO replace SQLITE_BUSY with SQLITE_LOCKED if request is from the same process

    switch(azure_fptr->lock_type)
    {
        case SQLITE_LOCK_NONE:
            // It is allowed to raise elsewhere from NONE
            assert(azure_fptr->shared_locks_count == 0);
            azure_fptr->lock_type = lock;
            if(lock == SQLITE_LOCK_SHARED)
                azure_fptr->shared_locks_count = 1;
            else
                azure_fptr->lock_task = tx_thread_identify();
            break;
        case SQLITE_LOCK_SHARED:
            assert(azure_fptr->shared_locks_count > 0);
            switch(lock)
            {
                case SQLITE_LOCK_SHARED:
                      azure_fptr->shared_locks_count++;
                    break;
                case SQLITE_LOCK_RESERVED:
                case SQLITE_LOCK_PENDING:
                    azure_fptr->shared_locks_count--;
                    azure_fptr->lock_type = lock;
                    azure_fptr->lock_task = tx_thread_identify();
                    break;
                case SQLITE_LOCK_EXCLUSIVE:
                    azure_fptr->shared_locks_count--;
                    azure_fptr->lock_task = tx_thread_identify();
                    if(azure_fptr->shared_locks_count)
                    {   // If there are still SHARED (read) locks we set PENDING instead to wait them go
                        azure_fptr->lock_type = SQLITE_LOCK_PENDING;
                        retval = SQLITE_BUSY; // And notify that we have not obtained EXCLUSIVE
                    }
                    else
                        azure_fptr->lock_type = SQLITE_LOCK_EXCLUSIVE;
                    break;
                default:
                    retval = SQLITE_ERROR; // May also use assert, SQLite stops on ERROR anyway
            }
            break;
        case SQLITE_LOCK_RESERVED:
            switch(lock)
            {
                case SQLITE_LOCK_SHARED: // SQLite does not check locks often, and this situation means someone wants to read despite
                    if(azure_fptr->lock_task == tx_thread_identify())
                        retval = SQLITE_ERROR;
                    else
                        azure_fptr->shared_locks_count++;
                    break;
                case SQLITE_LOCK_RESERVED: // Some process may even want to raise higher without a check
                    if(azure_fptr->lock_task != tx_thread_identify())
                        retval = SQLITE_BUSY;
                    break;
                case SQLITE_LOCK_PENDING:
                    if(azure_fptr->lock_task == tx_thread_identify())
                        azure_fptr->lock_type = SQLITE_LOCK_PENDING;
                    else
                        retval = SQLITE_BUSY;
                    break;
                case SQLITE_LOCK_EXCLUSIVE:
                    if(azure_fptr->lock_task != tx_thread_identify())
                        retval = SQLITE_BUSY;
                    else
                    {
                        if(azure_fptr->shared_locks_count)
                        {
                            azure_fptr->lock_type = SQLITE_LOCK_PENDING;
                            retval = SQLITE_BUSY;
                        }
                        else
                            azure_fptr->lock_type = SQLITE_LOCK_EXCLUSIVE;
                    }
                    break;
                default:
                    assert(0);
            }
            break;
        case SQLITE_LOCK_PENDING:
            switch(lock)
            {
                case SQLITE_LOCK_SHARED:
                    if(azure_fptr->lock_task == tx_thread_identify())
                        retval = SQLITE_ERROR;
                    else
                        retval = SQLITE_BUSY; // It is not allowed to obtain SHARED locks while PENDING anymore
                    break;
                case SQLITE_LOCK_RESERVED:
                    retval = SQLITE_BUSY;
                    break;
                case SQLITE_LOCK_PENDING:
                    if(azure_fptr->lock_task != tx_thread_identify())
                        retval = SQLITE_BUSY;
                    break;
                case SQLITE_LOCK_EXCLUSIVE:
                    if(azure_fptr->lock_task != tx_thread_identify())
                        retval = SQLITE_BUSY;
                    else
                    {
                        if(azure_fptr->shared_locks_count)
                            retval = SQLITE_BUSY;
                        else
                            azure_fptr->lock_type = SQLITE_LOCK_EXCLUSIVE;
                    }
                    break;
                default:
                    assert(0);
            }
            break;
        case SQLITE_LOCK_EXCLUSIVE:
            switch(lock)
            {
                case SQLITE_LOCK_SHARED:
                case SQLITE_LOCK_RESERVED:
                case SQLITE_LOCK_PENDING:
                    if(azure_fptr->lock_task == tx_thread_identify())
                        retval = SQLITE_ERROR;
                    else
                        retval = SQLITE_BUSY;
                    break;
                case SQLITE_LOCK_EXCLUSIVE:
                    if(azure_fptr->lock_task != tx_thread_identify())
                        retval = SQLITE_BUSY;
                    break;
                default:
                    assert(0);
            }
            break;
        default:
            assert(0);
    }

#if SQLITE3_AZURE_CONFIG_DEBUG > 1
    printf("xLock %s from %i to %i from process %i, %s%s", azure_fptr->fx_file_name, azure_fptr->lock_type, lock,
            (unsigned)tx_thread_identify(), retval == SQLITE_OK ? "SUCCESS" : "BUSY", newline);
#endif

    mutex_put(&(azure_fptr->mutex));

    return retval;
}

int xUnlock(sqlite3_file* fptr, int lock)
{
    assert(lock != SQLITE_LOCK_EXCLUSIVE); // xUnLock may only downgrade lock

	FX_FILE* const azure_fptr = convert_fptr(fptr);

    int retval = SQLITE_OK;

    mutex_get(&(azure_fptr->mutex), TX_WAIT_FOREVER);

#if SQLITE3_AZURE_CONFIG_DEBUG > 1
    printf("xUnLock %s to %i from process %i%s", azure_fptr->fx_file_name, lock, (unsigned)tx_thread_identify(), newline);
#endif

    switch(lock)
    {
        case SQLITE_LOCK_NONE:
            switch(azure_fptr->lock_type)
            {
                case SQLITE_LOCK_NONE:
                    break;
                case SQLITE_LOCK_SHARED:
                    assert(azure_fptr->shared_locks_count > 0);
                    if(--(azure_fptr->shared_locks_count) == 0)
                        azure_fptr->lock_type = SQLITE_LOCK_NONE;
                    break;
                case SQLITE_LOCK_RESERVED:
                case SQLITE_LOCK_PENDING:
                case SQLITE_LOCK_EXCLUSIVE:
                    if(azure_fptr->lock_task == tx_thread_identify())
                        if(azure_fptr->shared_locks_count)
                            azure_fptr->lock_type = SQLITE_LOCK_SHARED;
                        else
                            azure_fptr->lock_type = SQLITE_LOCK_NONE;
                    else
                        if(azure_fptr->shared_locks_count)
                            azure_fptr->shared_locks_count--;
                        /*else
                            retval = SQLITE_ERROR;*/
                    break;
                default:
                    assert(0);
            }
            break;
        case SQLITE_LOCK_SHARED:
            switch(azure_fptr->lock_type)
            {
                case SQLITE_LOCK_NONE:
                case SQLITE_LOCK_SHARED:
                    retval = SQLITE_ERROR;
                    break;
                case SQLITE_LOCK_RESERVED:
                case SQLITE_LOCK_PENDING:
                case SQLITE_LOCK_EXCLUSIVE:
                    if(azure_fptr->lock_task == tx_thread_identify())
                    {
                        azure_fptr->lock_type = SQLITE_LOCK_SHARED;
                        azure_fptr->shared_locks_count++;
                    }
                    else
                        retval = SQLITE_ERROR;
                    break;
                default:
                    assert(0);
            }
            break;
        default:
            assert(0);
    }

    mutex_put(&(azure_fptr->mutex));

    return retval;
}

int xCheckReservedLock(sqlite3_file* fptr, int *pResOut)
{
    assert(pResOut);

	FX_FILE* const azure_fptr = convert_fptr(fptr);

    mutex_get(&(azure_fptr->mutex), TX_WAIT_FOREVER);

    // Returns true if there is lock higher than SHARED
    *pResOut = (azure_fptr->lock_type >= SQLITE_LOCK_RESERVED);

    mutex_put(&(azure_fptr->mutex));

#if SQLITE3_AZURE_CONFIG_DEBUG > 2
    printf("xCheckReservedLock %s, got %i%s", azure_fptr->fx_file_name, *pResOut, newline);
#endif

    return SQLITE_OK;
}

int xFileControl(sqlite3_file* fptr, int op, void *pArg)
{
	FX_FILE* const azure_fptr = convert_fptr(fptr);

#if SQLITE3_AZURE_CONFIG_DEBUG > 2
    printf("xFileControl on file %s, code %i%s", azure_fptr->fx_file_name, op, newline);
#endif

    switch(op)
    {
       case SQLITE_FCNTL_LOCKSTATE:
           assert(pArg);
           *((int*)pArg) = azure_fptr->lock_type;
           return SQLITE_OK;
       case SQLITE_FCNTL_SIZE_HINT:
    	   assert(pArg);
    	   ULONG64 allocate = *((sqlite_int64*)pArg);
    	   if(allocate < azure_fptr->fx_file_current_file_size)
               return SQLITE_OK;
    	   allocate -= azure_fptr->fx_file_current_file_size;
    	   if(allocate < PREALLOCATE_MINIMUM)
               return SQLITE_OK;
           fx_file_extended_best_effort_allocate(azure_fptr, allocate, &allocate);
           return SQLITE_OK;
       case SQLITE_FCNTL_RESET_CACHE:
           return TranslateReturnValue(fx_media_cache_invalidate(azure_fptr->fx_file_media_ptr));
       case SQLITE_FCNTL_HAS_MOVED:
    	   assert(pArg);
           *((int*)pArg) = 0;
           return SQLITE_OK;
       default:
           return SQLITE_NOTFOUND;
    }
}

int xSectorSize(sqlite3_file* fptr)
{
	FX_FILE* const azure_fptr = convert_fptr(fptr);

    return azure_fptr->fx_file_media_ptr->fx_media_bytes_per_sector;
}

int xDeviceCharacteristics(sqlite3_file*)
{
    return SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN /*| SQLITE_IOCAP_SAFE_APPEND seems good but still need to test it*/;
}

const sqlite3_io_methods azure_file_methods = {
        .iVersion               = 1,
        .xClose                 = xClose,
        .xRead                  = xRead,
        .xWrite                 = xWrite,
        .xTruncate              = xTruncate,
        .xSync                  = xSync,
        .xFileSize              = xFileSize,
        .xLock                  = xLock,
        .xUnlock                = xUnlock,
        .xCheckReservedLock     = xCheckReservedLock,
        .xFileControl           = xFileControl,
        .xSectorSize            = xSectorSize,
        .xDeviceCharacteristics = xDeviceCharacteristics,
          /* Methods above are valid for version 1 */
        /*.xShmMap = xShmMap,
        .xShmLock = xShmLock,
        .xShmBarrier = xShmBarrier,
        .xShmUnmap = xShmUnmap,*/
          /* Methods above are valid for version 2 */
        /*.xFetch = xFetch,
        .xUnfetch = xUnfetch*/
};

/* ---------------------------- Memory management ------------------------------------ */
// Not tested, used static buffers instead
#if !SQLITE3_AZURE_CONFIG_STATIC_POOL && SQLITE3_AZURE_CONFIG_DYNAMIC_POOL

struct malloc_size{
    void* address;
    int   size;
};

struct malloc_size malloc_sizes[SQLITE3_AZURE_CONFIG_TRACKABLE_MALLOCS];

void* xMalloc(int size)
{
    void* result;

    if(tx_byte_allocate(SQLITE3_AZURE_CONFIG_DYNAMIC_POOL, &result, size, TX_WAIT_FOREVER) == TX_SUCCESS)
    {
        for(unsigned i = 0; i < SQLITE3_AZURE_CONFIG_TRACKABLE_MALLOCS; i++)
            if(malloc_sizes[i].address == NULL)
            {
                malloc_sizes[i].address = result;
                malloc_sizes[i].size = size;
                break;
            }
        return result;
    }

    return NULL;
}

void xFree(void* memptr)
{
	assert(memptr);

    tx_byte_release(memptr);

    for(unsigned i = 0; i < SQLITE3_AZURE_CONFIG_TRACKABLE_MALLOCS; i++)
        if(malloc_sizes[i].address == memptr)
        {
            malloc_sizes[i].address = NULL;
            return;
        }
}

void* xRealloc(void* memptr, int size)
{
	assert(memptr);

    void* newmem = xMalloc(size);
    if(newmem == NULL)
        return NULL;
    memcpy(newmem, memptr, size);
    xFree(memptr);
    return newmem;
}

int xSize(void* memptr)
{
	assert(memptr);

    for(unsigned i = 0; i < SQLITE3_AZURE_CONFIG_TRACKABLE_MALLOCS; i++)
        if(malloc_sizes[i].address == memptr)
            return malloc_sizes[i].size;

    return 0;
}

int xRoundup(int size)
{
    return size;
}

int xInit(void*)
{
    for(unsigned i = 0; i < SQLITE3_AZURE_CONFIG_TRACKABLE_MALLOCS; i++)
        malloc_sizes[i].address = NULL;
    return SQLITE_OK;
}

void xShutdown(void*)
{
    for(unsigned i = 0; i < SQLITE3_AZURE_CONFIG_TRACKABLE_MALLOCS; i++)
        if(malloc_sizes[i].address != NULL)
            xFree(malloc_sizes[i].address);
}

const sqlite3_mem_methods mem_methods = {
          .xMalloc   = xMalloc,     /* Memory allocation function */
          .xFree     = xFree,         /* Free a prior allocation */
          .xRealloc  = xRealloc,   /* Resize an allocation */
          .xSize     = xSize,         /* Return the size of an allocation */
          .xRoundup  = xRoundup,   /* Round up request size to allocation size */
          .xInit     = xInit,         /* Initialize the memory allocator */
          .xShutdown = xShutdown,  /* Deinitialize the memory allocator */
          .pAppData  = NULL        /* Argument to xInit() and xShutdown() */
};

#endif

/* ---------------------------- Mutexes ---------------------------------------------- */

#if SQLITE_THREADSAFE

struct sqlite3_mutex { TX_MUTEX mutex; };

sqlite3_mutex sqlite3_mutexes[SQLITE3_AZURE_CONFIG_STATIC_MUTEXES];
#if 1
const char* sqlite3_mutex_names[SQLITE3_AZURE_CONFIG_STATIC_MUTEXES] = {
                  "SQLITE_MUTEX_STATIC_MAIN", "SQLITE_MUTEX_STATIC_MEM",
                  "SQLITE_MUTEX_STATIC_OPEN", "SQLITE_MUTEX_STATIC_PRNG", "SQLITE_MUTEX_STATIC_LRU",
				  "SQLITE_MUTEX_STATIC_PMEM", "SQLITE_MUTEX_STATIC_APP1",
				  "SQLITE_MUTEX_STATIC_APP2", "SQLITE_MUTEX_STATIC_APP3", "SQLITE_MUTEX_STATIC_VFS1",
				  "SQLITE_MUTEX_STATIC_VFS2", "SQLITE_MUTEX_STATIC_VFS3" };
#else
const char* const nullstr = "";
const char* sqlite3_mutex_names[SQLITE3_AZURE_CONFIG_STATIC_MUTEXES] = {
		nullstr, nullstr,
		nullstr, nullstr, nullstr,
		nullstr, nullstr,
		nullstr, nullstr, nullstr,
		nullstr, nullstr };
#endif

int xMutexInit(void)
{
    for(unsigned i = 0; i < SQLITE3_AZURE_CONFIG_STATIC_MUTEXES; i++)
        if(mutex_create(&(sqlite3_mutexes[i].mutex), (char*)sqlite3_mutex_names[i], TX_NO_INHERIT) != TX_SUCCESS)
            ;//return SQLITE_ERROR; // Should not normally happen, use for debug

    return SQLITE_OK;
}

int xMutexEnd(void)
{
    for(unsigned i = 0; i < SQLITE3_AZURE_CONFIG_STATIC_MUTEXES; i++)
        if(mutex_delete(&(sqlite3_mutexes[i].mutex)) != TX_SUCCESS)
            ; // return SQLITE_ERROR; // Should not normally happen, just for debug

    return SQLITE_OK;
}

sqlite3_mutex* xMutexAlloc(int type)
{

    if(type < SQLITE_MUTEX_STATIC_MAIN)
    {
        sqlite3_mutex* mutex_ptr = sqlite3_malloc(sizeof(sqlite3_mutex));

        if(mutex_ptr == NULL)
            return NULL;

        if(mutex_create(&(mutex_ptr->mutex), "SQLIte Azure dynamic mutex", TX_NO_INHERIT) != TX_SUCCESS)
        {
            sqlite3_free(mutex_ptr);
            return NULL;
        }

        return mutex_ptr;
    }

    type -= SQLITE_MUTEX_STATIC_MAIN;

    if(type < SQLITE3_AZURE_CONFIG_STATIC_MUTEXES)
        return &sqlite3_mutexes[type];

    return NULL;
}

void xMutexFree(sqlite3_mutex* mutex)
{
	assert(mutex);

    if((mutex < sqlite3_mutexes) || (mutex > &sqlite3_mutexes[SQLITE3_AZURE_CONFIG_STATIC_MUTEXES - 1]))
    {
        if(FX_SUCCESS == mutex_delete(&(mutex->mutex))) // Prevents heap crash if the pointer is wrong
            sqlite3_free(mutex);
    }
}

void xMutexEnter(sqlite3_mutex* mutex)
{
	assert(mutex);

    tx_mutex_get(&(mutex->mutex), TX_WAIT_FOREVER);
}

int xMutexTry(sqlite3_mutex* mutex)
{
	assert(mutex);

    if(tx_mutex_get(&(mutex->mutex), TX_NO_WAIT) == TX_WAIT_ERROR)
        return SQLITE_BUSY;

    return SQLITE_OK;
}

void xMutexLeave(sqlite3_mutex* mutex)
{
	assert(mutex);

    tx_mutex_put(&(mutex->mutex));
}

int xMutexHeld(sqlite3_mutex* mutex)
{
	assert(mutex);

    return mutex->mutex.tx_mutex_ownership_count;
}

int xMutexNotheld(sqlite3_mutex* mutex) { return !xMutexHeld(mutex); }

const sqlite3_mutex_methods azure_mutexes = {
  .xMutexInit    = xMutexInit,
  .xMutexEnd     = xMutexEnd,
  .xMutexAlloc   = xMutexAlloc,
  .xMutexFree    = xMutexFree,
  .xMutexEnter   = xMutexEnter,
  .xMutexTry     = xMutexTry,
  .xMutexLeave   = xMutexLeave,
  .xMutexHeld    = xMutexHeld,
  .xMutexNotheld = xMutexNotheld
};

#endif

/* ---------------------------- Initialization --------------------------------------- */

void errorLogCallback(void *pArg, int iErrCode, const char *zMsg)
{
	// Here and in all debug output printf is used
	// On MCU printf mostly prints to UART, so it helps
	// stderr is unbuffered and may cause errors
    printf("(%d) %s\r\n", iErrCode, zMsg);
}

void sqlite3_azure_init(FX_MEDIA* media_ptr, sqlite3_int64 (*datetime64)(void), int (*random_generator)(void))
{
    assert(media_ptr);

    sqlite3_media_ptr = media_ptr;

    if(SQLITE3_AZURE_CONFIG_STATIC_POOL)
       sqlite3_config(SQLITE_CONFIG_HEAP, SQLITE3_AZURE_CONFIG_STATIC_POOL, SQLITE3_AZURE_CONFIG_MEMORY_POOL_SIZE, 64);

#if !SQLITE3_AZURE_CONFIG_STATIC_POOL && SQLITE3_AZURE_CONFIG_DYNAMIC_POOL
    sqlite3_config(SQLITE_CONFIG_MALLOC, &mem_methods);
#endif

    if(SQLITE3_AZURE_CONFIG_PAGE_POOL)
    {
        int header_size;
        sqlite3_config(SQLITE_CONFIG_PCACHE_HDRSZ, &header_size);
        if(header_size & (SQLITE3_AZURE_CONFIG_PAGE_POOL_ALIGNMENT - 1))
        {
            header_size += SQLITE3_AZURE_CONFIG_PAGE_POOL_ALIGNMENT;
            header_size	&= ~(SQLITE3_AZURE_CONFIG_PAGE_POOL_ALIGNMENT - 1);
        }
        sqlite3_config(SQLITE_CONFIG_PAGECACHE,
                       SQLITE3_AZURE_CONFIG_PAGE_POOL,
                       SQLITE3_AZURE_CONFIG_MAX_PAGE_SIZE + header_size,
                       SQLITE3_AZURE_CONFIG_PAGE_POOL_SIZE / (SQLITE3_AZURE_CONFIG_MAX_PAGE_SIZE + header_size));
    }

    sqlite3_config(SQLITE_CONFIG_SMALL_MALLOC, SQLITE3_AZURE_CONFIG_SMALL_MALLOC);

    // Important, without this I could not fit in MCU memory
    // Number of buffers seems to be enough, so if you have extra memory raise size first
    sqlite3_config(SQLITE_CONFIG_LOOKASIDE, 64, 64);

    if(SQLITE3_AZURE_CONFIG_SCRATCH)
        sqlite3_config(SQLITE_CONFIG_SCRATCH, SQLITE3_AZURE_CONFIG_SCRATCH, SQLITE3_AZURE_CONFIG_SCRATCH_SIZE);

#if SQLITE_THREADSAFE
    sqlite3_config(SQLITE_CONFIG_MUTEX, &azure_mutexes);
#endif

#if SQLITE3_AZURE_CONFIG_DEBUG
    sqlite3_config(SQLITE_CONFIG_LOG, errorLogCallback, NULL);
    sqlite3_config(SQLITE_CONFIG_MEMSTATUS, -1);
#endif

    mutex_create(&openclose, "SQLIte Azure file open/close mutex", TX_NO_INHERIT);

    if(datetime64)
        sqlite3_time64 = datetime64;

    if(random_generator)
        sqlite3_xrandomness = random_generator;

    sqlite3_initialize(); // Important to do before VFS registration

    sqlite3_vfs_register(&azure_vfs, 1);
}

//int sqlite3_os_init(void)
//{
//    return SQLITE_OK;
//}
//
//int sqlite3_os_end(void)
//{
//    return SQLITE_OK;
//}
