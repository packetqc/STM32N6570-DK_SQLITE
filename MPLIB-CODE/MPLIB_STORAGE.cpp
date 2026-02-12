/*
 * MPLIB_STORAGE.cpp
 *
 *  Created on: Feb 4, 2026
 *      Author: Packet
 */

#include <MPLIB_STORAGE.h>


#include "stdbool.h"
#include "stddef.h"

#include "stm32n6xx_hal.h"
#include "stm32n6xx_hal_rtc.h"
#include "stm32n6xx_hal_sd.h"
#include "stm32n6xx_hal_dma.h"

extern "C" {
	#include "sqlite3.h"
	#include "sqlite3_azure.h"
	#include "app_filex.h"
	#include "tx_api.h"
	#include "main.h"
}

//=======================================================================================
//
//=======================================================================================
int MPLIB_STORAGE::iSTORAGE = 0;
MPLIB_STORAGE *MPLIB_STORAGE::instance=NULL;
char* MPLIB_STORAGE::name_singleton = (char*)malloc(CAT_LENGTH * sizeof(char));

//=======================================================================================
//
//=======================================================================================
MPLIB_STORAGE *STORAGE = MPLIB_STORAGE::CreateInstance();

// Static pointer for interrupt callback
static MPLIB_STORAGE* storage_instance_for_dma = nullptr;

//=======================================================================================
//
//=======================================================================================
TX_THREAD storage_thread;
TX_THREAD ingestion_thread;
TX_THREAD simulator_thread;

TX_MUTEX sd_io_mutex;
TX_MUTEX db_mutex;

//TX_EVENT_FLAGS_GROUP db_flags;
//TX_EVENT_FLAGS_GROUP sd_events;
TX_EVENT_FLAGS_GROUP staging_events;

// For interrupt-driven version (optional)
TX_SEMAPHORE dma_complete_sem;

volatile bool dma_transfer_complete;

// DMA handle for PSRAM â†’ SRAM transfers
DMA_HandleTypeDef hdma_mem2mem;

//=======================================================================================
//
//=======================================================================================
extern FX_MEDIA sdio_disk;
extern SD_HandleTypeDef hsd2;
extern RNG_HandleTypeDef hrng;

// External DMA handle from CubeMX generated code
extern DMA_HandleTypeDef handle_GPDMA1_Channel0;

extern "C" {
    extern uint8_t __psram_pcache_start, __psram_pcache_end;
    extern uint8_t __psram_heap_start, __psram_heap_end;
    extern uint32_t __psram_logs_start;
    extern uint32_t __psram_logs_end;
}

//=======================================================================================
//
//=======================================================================================
__attribute__((section(".SqlPoolSection"),aligned(32))) uint8_t storage_stack[STORAGE_STACK_SIZE];

__attribute__((section(".SqlPoolSection"),aligned(32))) uint8_t simulator_stack[SIMULATOR_STACK_SIZE];

__attribute__((section(".SqlPoolSection"),aligned(32))) uint8_t ingestion_stack[INGESTION_STACK_SIZE];

__attribute__((section(".SqlPoolSection"), aligned(32))) static uint8_t sram_landing_zone[SRAM_LANDING_SIZE];

// SUPERPOWER: 1 MB heap (was 512 KB) — headroom for memsys5 fragmentation at scale
__attribute__((section(".psram_data"), aligned(32))) char sqlite_heap[1024 * 1024];

// SUPERPOWER: 4 MB page cache (was 393 KB / 96 slots)
// At 4M rows the B-tree has 5-6 levels; all interior pages must stay hot.
// Slot size = page_size(4096) + pcache header(~256) = 4352 bytes per slot
// 4 MB / 4352 = ~965 slots — enough to hold one full buffer's B-tree pages in RAM
__attribute__((section(".psram_cache"), aligned(32))) char sqlite_pcache[4 * 1024 * 1024];

__attribute__((section(".psram_logs"), aligned(32))) static DS_LOG_STRUCT psram_buffer_A[LOGS_PER_BUFFER];

__attribute__((section(".psram_logs"), aligned(32))) static DS_LOG_STRUCT psram_buffer_B[LOGS_PER_BUFFER];

//=======================================================================================
//
//=======================================================================================
const char* DB_NAME = "logs.db";

//=======================================================================================
// THREADS CREATION
//=======================================================================================
void StartStorageServices(ULONG thread_input) {
    UINT tx_status = TX_SUCCESS;

    STORAGE->init();

    // SIMULATOR: Priority 15 (lowest — yields to ingestor)
    tx_status = tx_thread_create(
        &simulator_thread,
        "Log Simulator",
        simulator_thread_entry,
        0,
        simulator_stack, sizeof(simulator_stack),
        15, 15, 1, 0
    );

    if (tx_status != TX_SUCCESS)
    {
      printf("ERROR TO START SIMULATOR THREAD: %d\n", tx_status);
    }
    else {
        tx_thread_resume(&simulator_thread);
        printf("\nOK SIMULATOR STARTED\n");
    }

    // INGESTION: Priority 5 (highest — preempts simulator for SD I/O)
    tx_status = tx_thread_create(
        &ingestion_thread,
        (CHAR*)"SQLite Ingestion",
		ingestion_direct_thread_entry,
        0,
        ingestion_stack, INGESTION_STACK_SIZE,
        5, 5, 0, 0
    );

    if (tx_status != TX_SUCCESS)
    {
        printf("ERROR TO START INGESTION THREAD: %d\n", tx_status);
    }
    else {
        tx_thread_resume(&ingestion_thread);
        printf("\nOK INGESTOR STARTED\n");
    }

    printf("\nOK DB STORAGE STARTING SERVICES\n");

    // STORAGE/WORK: Priority 8 (set in main service loop)
    for(;;) {
//        STORAGE->service(); //FOR INGESTION WITH LANDING ZONE AND RAW FILES ONLY
//        tx_thread_sleep(1);
        tx_thread_sleep(30);
    }
}

//=======================================================================================
//
//=======================================================================================
void simulator_thread_entry(ULONG instance_ptr) {
    STORAGE->simulator();
}

extern "C" void ingestion_thread_entry(unsigned long thread_input) {
    STORAGE->ingestor(0);
}

extern "C" void ingestion_direct_thread_entry(unsigned long thread_input) {
    STORAGE->ingestor_direct(0);
}

//=======================================================================================
// INGESTION STRATEGY WITH LANDING ZONE AND RAW FILES ONLY
//=======================================================================================
uint32_t produce_idx = 0; // Next file to write
uint32_t consume_idx = 0; // Next file to read
const uint32_t MAX_RAW_FILES = 40;
TX_SEMAPHORE sem_raw_files; // Count of files ready for SQLite

//=======================================================================================
//
//=======================================================================================

/* This function is called automatically by sqlite3_initialize() when SQLITE_OS_OTHER=1 is defined. */
int sqlite3_os_init(void)
{
	int status = SQLITE_OK;

	return status;
}

/* This is called by sqlite3_shutdown() */
int sqlite3_os_end(void)
{
  return SQLITE_OK;
}

//=======================================================================================
// GLOBAL PERFORMANCE COUNTERS (add to top of .cpp file)
//=======================================================================================

// Simulator stats (already exists, enhance it)
static uint32_t sim_total_logs = 0;
static uint32_t sim_last_count = 0;
static uint32_t sim_last_time = 0;

// Storage stats
static uint32_t stor_total_logs = 0;
static uint32_t stor_last_count = 0;
static uint32_t stor_last_time = 0;

// Ingestion stats
static uint32_t ing_total_logs = 0;
static uint32_t ing_last_count = 0;
static uint32_t ing_last_time = 0;
static uint32_t ing_total_skipped = 0;

//=======================================================================================
//
//=======================================================================================
void MPLIB_STORAGE::setStart(bool value)
{
	started = value;
}

//=======================================================================================
//
//=======================================================================================
void MPLIB_STORAGE::init_psram()
{
	printf("\nOK [INIT] Initializing PSRAM buffers...\n");

	// ================================================================
	// CRITICAL: Manual zero-fill with barriers for PSRAM
	// ================================================================

	// Zero buffer A
	for (uint32_t i = 0; i < LOGS_PER_BUFFER; i++) {
		volatile DS_LOG_STRUCT* log = &psram_buffer_A[i];
		log->log_index = 0;
		log->token = 0;
		log->local_log_index = 0;
		log->timestamp_at_store = 0;
		log->timestamp_at_log = 0;
		log->severity = 0;
		memset((void*)log->category, 0, sizeof(log->category));
		memset((void*)log->message, 0, sizeof(log->message));
		memset((void*)log->reserved, 0, sizeof(log->reserved));
	}

	__DSB();  // Ensure all writes are complete
	__ISB();  // Instruction barrier

	// Zero buffer B
	for (uint32_t i = 0; i < LOGS_PER_BUFFER; i++) {
		volatile DS_LOG_STRUCT* log = &psram_buffer_B[i];
		log->log_index = 0;
		log->token = 0;
		log->local_log_index = 0;
		log->timestamp_at_store = 0;
		log->timestamp_at_log = 0;
		log->severity = 0;
		memset((void*)log->category, 0, sizeof(log->category));
		memset((void*)log->message, 0, sizeof(log->message));
		memset((void*)log->reserved, 0, sizeof(log->reserved));
	}

	__DSB();  // Ensure all writes are complete
	__ISB();  // Instruction barrier

	printf("\nOK [INIT] PSRAM buffers manually zeroed (%lu logs each)\n", LOGS_PER_BUFFER);

	// Verify zeroing worked
	if (psram_buffer_A[0].log_index != 0 || psram_buffer_B[0].log_index != 0) {
		printf("\nERROR [INIT] PSRAM zero verification FAILED!\n");
		printf("  Buffer A[0].log_index = 0x%08lX (expected 0)\n", psram_buffer_A[0].log_index);
		printf("  Buffer B[0].log_index = 0x%08lX (expected 0)\n", psram_buffer_B[0].log_index);
	} else {
		printf("\nOK [INIT] PSRAM zero verification PASSED\n");
	}
}

//=======================================================================================
//
//=======================================================================================
bool MPLIB_STORAGE::init() {
    UINT tx_status;
    int rc;

    printf("\nOK [INIT] Performing Global SQLite Configuration...\n");

    // CRITICAL: shutdown first to allow reconfiguration
    sqlite3_shutdown();

    // CONFIG 1: Use PSRAM for the Page Cache
    // CRITICAL: slot size MUST be page_size + header overhead (~256 bytes).
    // Old value of 4096 was too small — SQLite silently fell back to the heap for every page!
    #define PCACHE_SLOT_SIZE (4096 + 256)
    rc = sqlite3_config(SQLITE_CONFIG_PAGECACHE, sqlite_pcache, PCACHE_SLOT_SIZE, (int)(sizeof(sqlite_pcache) / PCACHE_SLOT_SIZE));
    if (rc != SQLITE_OK) printf("\nWARN [INIT] PageCache Config Failed: %d", rc);

    // CONFIG 2: Use PSRAM for the Heap (memsys5)
    rc = sqlite3_config(SQLITE_CONFIG_HEAP, sqlite_heap, (int)sizeof(sqlite_heap), 64);
    if (rc != SQLITE_OK) printf("\nWARN [INIT] Heap Config Failed: %d", rc);

    // CONFIG 3: Enable status tracking for your terminal stats
    sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 1);

    // INITIALIZE: Locks the config and prepares the engine
    rc = sqlite3_initialize();
    if (rc != SQLITE_OK) {
        printf("\nERROR [INIT] SQLite init failed: %d\n", rc);
        return false;
    }
    printf("\nOK [INIT] SQLite Engine Initialized with PSRAM Cache\n");

    // --- Hardware & Semaphore Setup ---
    memcpy(&hdma_mem2mem, &handle_GPDMA1_Channel0, sizeof(DMA_HandleTypeDef));
    storage_instance_for_dma = this;

    tx_status = tx_semaphore_create(&dma_complete_sem, "DMA Complete", 0);
    if (tx_status != TX_SUCCESS) return false;

    init_psram();
    buffer_A = psram_buffer_A;
    buffer_B = psram_buffer_B;
    active_fill_buffer = buffer_A;

    tx_status = tx_event_flags_create(&staging_events, "Staging Events");
    if (tx_status != TX_SUCCESS) return false;

    tx_event_flags_set(&staging_events, 0, TX_AND);  // Clear all bits
    tx_event_flags_set(&staging_events, FLAG_BUF_A_FREE | FLAG_BUF_B_FREE, TX_OR);  // Both buffers start free

    tx_status = tx_mutex_create(&sd_io_mutex, "SD I/O Mutex", TX_NO_INHERIT);
    tx_status = tx_mutex_create(&db_mutex, "DB Mutex", TX_NO_INHERIT);
    tx_status = tx_semaphore_create(&sem_raw_files, "Raw Files Semaphore", 0);

    delete_database_files();
    printf("\nOK [INIT] Starting database: %s\n", DB_NAME);

    // OPEN & TUNE
    rc = sqlite3_open(DB_NAME, &db);
    if (rc != SQLITE_OK) {
        printf("\nERROR [INIT] Failed to open DB: %s\n", sqlite3_errmsg(db));
        return false;
    }

    tuneDbConfig();

    if (!createTable()) {
        printf("\nERROR [INIT] Failed to create table\n");
        return false;
    }

    // Finalize and Close so Ingestor thread can take over
    if (insert_stmt) { sqlite3_finalize(insert_stmt); insert_stmt = nullptr; }
    if (db) {
        sqlite3_close(db);
        db = nullptr;
        printf("\nOK [INIT] Database closed for Ingestor takeover\n");
    }

    printf("\nOK [INIT] SQLite Engine Ready\n");
    this->setStart(true);
    return true;
}

//=======================================================================================
//
//=======================================================================================
void MPLIB_STORAGE::simulator() {
    DS_LOG_STRUCT test_log;
    uint32_t counter = 0;
    const char *sim_name = "SIMULATOR";
    int cur, hi;

    sim_last_time = tx_time_get();
    sim_last_count = 0; // Ensure reset
    ing_last_count = 0; // Ensure reset

    printf("\nOK [SIMULATOR] Simulator Online - OPTIMIZED MODE\n");

    while(1) {
        // ... [Log filling logic remains the same] ...
        test_log.log_index = counter;
        snprintf(test_log.message, LOG_LENGTH, "Burst #%lu", counter);
        snprintf(test_log.category, CAT_LENGTH, "%s", sim_name);
        test_log.token = 13131;
        test_log.local_log_index = 0;
        test_log.timestamp_at_store = 0;
        test_log.timestamp_at_log = tx_time_get();
        test_log.severity = 1;

        counter++;
        sim_total_logs++;

        this->captureLog(test_log);

        uint32_t current_time = tx_time_get();

        // Stats loop runs every 5 seconds (5000 ticks)
        if (current_time - sim_last_time >= 5000) {
            // FIX: Divide by 5 to get per-second average over the 5s window
            uint32_t sim_logs_this_sec = (sim_total_logs - sim_last_count) / 5;
            uint32_t ing_logs_this_sec = (ing_total_logs - ing_last_count) / 5;

            printf("\n--- STATS BLOCK ---------------------------------------------------------------------------");
            printf("\n[STATS] SIMULATOR : %5lu logs/sec | Total: %7lu",
                   sim_logs_this_sec, sim_total_logs);
            printf("\n[STATS] INGESTION : %5lu logs/sec | Total: %7lu (Skipped: %lu)",
                   ing_logs_this_sec, ing_total_logs, ing_total_skipped);

            // FIX: Use ing_total_logs to show what is actually waiting in PSRAM
            uint32_t pending_in_psram = 0;
            if (sim_total_logs > ing_total_logs) {
                pending_in_psram = sim_total_logs - ing_total_logs;
            }

            printf("\n[STATS] PSRAM     : %lu logs pending write", pending_in_psram);

            sqlite3_status(SQLITE_STATUS_MEMORY_USED, &cur, &hi, 0);
            printf("\n[STATS] SQLite Mem: %d / %d bytes", cur, (int)sizeof(sqlite_heap));
            printf("\n--- STATS BLOCK ---------------------------------------------------------------------------\n");

            sim_last_time = current_time;
            sim_last_count = sim_total_logs;
            ing_last_count = ing_total_logs;
        }

        // Keep your timing logic
        if (counter % 20 == 0) {
            tx_thread_sleep(1);
        }
//        tx_thread_sleep(1);
    }
}

//=======================================================================================
//
//=======================================================================================
void MPLIB_STORAGE::work() {
    ULONG actual_flags;
    char raw_filename[32];

    stor_last_time = tx_time_get(); // Initialize

    printf("\nOK [STORAGE] service work thread loop started\n");  // ADD THIS

    while(1) {
//    	printf("\nDEBUG [STORAGE] Waiting for buffer flags...\n");

        tx_event_flags_get(&staging_events, 0x03, TX_OR_CLEAR, &actual_flags, TX_WAIT_FOREVER);

        printf("\nDEBUG [STORAGE] Woke up! actual_flags=0x%02lX\n", actual_flags);

        if (produce_idx - consume_idx >= MAX_RAW_FILES) {
            printf("\nCRITICAL [STORAGE] Queue Full! Stalling Simulator...\n");
            tx_thread_suspend(&simulator_thread);
            printf("\nOK [STORAGE] Simulator suspended\n");
        }

//        DS_LOG_STRUCT* src = (actual_flags & 0x01) ? psram_buffer_A : psram_buffer_B;
//        snprintf(raw_filename, sizeof(raw_filename), "batch_%lu.raw", produce_idx % MAX_RAW_FILES);
        DS_LOG_STRUCT* src = (actual_flags & 0x01) ? psram_buffer_A : psram_buffer_B;
        uint32_t actual_count = (actual_flags & 0x01) ? buffer_A_count : buffer_B_count;

        if (actual_count == 0) actual_count = LOGS_PER_BUFFER;  // Safety

        snprintf(raw_filename, sizeof(raw_filename), "batch_%lu.raw", produce_idx % MAX_RAW_FILES);

        uint32_t start_time = tx_time_get();

        if (this->writeRawFile(raw_filename, src, actual_count) == FX_SUCCESS) {
            uint32_t write_time = tx_time_get() - start_time;

            stor_total_logs += LOGS_PER_BUFFER;

            produce_idx++;
            tx_semaphore_put(&sem_raw_files);

            printf("\nOK [STORAGE] batch_%lu.raw written (%lu ms, %lu logs/sec instantaneous)\n",
                   produce_idx - 1,
                   write_time,
                   write_time > 0 ? (LOGS_PER_BUFFER * 1000 / write_time) : 0);
        }

        tx_thread_relinquish();
    }
}

//=======================================================================================
//
//=======================================================================================
void MPLIB_STORAGE::tuneDbConfig() {
    if (db == nullptr) return;

    char* zErrMsg = nullptr;
    const char* pragmas[] = {
        "PRAGMA page_size = 4096;",            // MUST match config sz (4096)
        "PRAGMA journal_mode = WAL;",
        "PRAGMA synchronous = OFF;",           // No fsyncs — max throughput (data loss on power-fail OK)
        "PRAGMA cache_size = -4096;",          // 4 MiB — keep ALL B-tree interior pages hot in PSRAM
        "PRAGMA locking_mode = EXCLUSIVE;",
        "PRAGMA temp_store = MEMORY;",
        "PRAGMA journal_size_limit = 4194304;",
        "PRAGMA wal_autocheckpoint = 0;",      // Disable auto-checkpoint; we do manual PASSIVE every 5 buffers
        "PRAGMA auto_vacuum = NONE;"
    };

    printf("\nOK [DB_CONFIG] Applying performance pragmas...\n");

    for (const char* sql : pragmas) {
        int rc = sqlite3_exec(db, sql, nullptr, nullptr, &zErrMsg);
        if (rc != SQLITE_OK) {
            printf("\nWARN [DB_CONFIG] Failed: %s -> %s\n", sql, zErrMsg ? zErrMsg : "err");
            if (zErrMsg) { sqlite3_free(zErrMsg); zErrMsg = nullptr; }
        }
    }
    printf("\nOK [DB_CONFIG] Storage-optimized configuration active\n");
}

//=======================================================================================
//
//=======================================================================================
void MPLIB_STORAGE::ingestor(ULONG thread_input) {
    char raw_filename[32];
    UINT state;

    printf("\nOK [INGESTION] SQLite Ingestor Thread Online (OPTIMIZED)\n");

    // ================================================================
    // CRITICAL: Open database in THIS thread (ingestion thread)
    // ================================================================
    printf("\nOK [INGESTION] Opening database in ingestion thread...\n");

    int rc = sqlite3_open(DB_NAME, &db);
    if (rc != SQLITE_OK) {
        printf("\nERROR [INGESTION] Failed to open DB: %s\n", sqlite3_errmsg(db));
        printf("\nFATAL [INGESTION] Cannot proceed without database!\n");
        return;
    }

    // Apply database configuration
    tuneDbConfig();
    printf("\nOK [INGESTION] Database configuration applied\n");

    // Prepare INSERT statement in THIS thread
    const char *sql = "INSERT INTO ds_logs (log_index, message, category, token, local_log_index, timestamp_at_store, timestamp_at_log, severity) VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    rc = sqlite3_prepare_v2(db, sql, -1, &insert_stmt, nullptr);
    if (rc != SQLITE_OK) {
        printf("\nERROR [INGESTION] Statement Prep Fail: %s\n", sqlite3_errmsg(db));
        printf("\nFATAL [INGESTION] Cannot proceed without prepared statement!\n");
        sqlite3_close_v2(db);
        db = nullptr;
        return;
    }
    printf("\nOK [INGESTION] Prepared statement configured\n");

    // Initialize stats timer
    ing_last_time = tx_time_get();

    // ================================================================
    // Main Ingestion Loop
    // ================================================================
    while(1) {
        // Wait for a file to be ready
    	printf("\nOK [INGESTION] Waiting for a file to be ready\n");
        tx_semaphore_get(&sem_raw_files, TX_WAIT_FOREVER);

        // ============================================================
        // CHECK IF DATABASE NEEDS REOPENING (after recovery)
        // ============================================================
        if (db == nullptr) {
            printf("\n--- STATS BLOCK ---------------------------------------------------------------------------");
            printf("\n[INGESTION] Database handle is null - reopening after recovery...");
            printf("\n--- STATS BLOCK ---------------------------------------------------------------------------\n");

            rc = sqlite3_open(DB_NAME, &db);
            if (rc != SQLITE_OK) {
                printf("\nERROR [INGESTION] Failed to reopen DB: %s !!!\n", sqlite3_errmsg(db));
                printf("\nINFO [INGESTION] Retrying in 1 second...\n");
                tx_semaphore_put(&sem_raw_files);
                tx_thread_sleep(1000);
                continue;
            }

            tuneDbConfig();
            printf("\n[INGESTION] Database configuration reapplied\n");

            rc = sqlite3_prepare_v2(db, sql, -1, &insert_stmt, nullptr);
            if (rc != SQLITE_OK) {
                printf("\nERROR [INGESTION] Failed to recreate statement: %s !!!\n", sqlite3_errmsg(db));
                sqlite3_close_v2(db);
                db = nullptr;
                tx_semaphore_put(&sem_raw_files);
                tx_thread_sleep(1000);
                continue;
            }

            printf("\n--- STATS BLOCK ---------------------------------------------------------------------------");
            printf("\n[INGESTION] Database reopened and ready!");
            printf("\n--- STATS BLOCK ---------------------------------------------------------------------------\n");
        }

        // ============================================================
        // Process Next File
        // ============================================================
        snprintf(raw_filename, sizeof(raw_filename), "batch_%lu.raw", consume_idx % MAX_RAW_FILES);

        // Calculate queue status BEFORE processing
        uint32_t files_in_queue = produce_idx - consume_idx;

        printf("\nOK [INGESTION] Processing batch_%lu.raw (Queue: %lu waiting / %lu total processed)\n",
               consume_idx, files_in_queue, consume_idx);

        uint32_t start_time = tx_time_get();

        // Track counts before processing
        uint32_t logs_before = ing_total_logs;
        uint32_t skipped_before = ing_total_skipped;

        if (this->ingestRawToSQLite(raw_filename, &state)) {
            // ========================================================
            // SUCCESS - Calculate actual counts
            // ========================================================
            uint32_t ingest_time = tx_time_get() - start_time;

            uint32_t actual_ingested = ing_total_logs - logs_before;
            uint32_t actual_skipped = ing_total_skipped - skipped_before;

            consume_idx++;

            // Calculate queue status AFTER processing
            uint32_t files_remaining = produce_idx - consume_idx;

            printf("\n--- STATS BLOCK ---------------------------------------------------------------------------");
            printf("\nOK [INGESTION] batch_%lu completed:", consume_idx - 1);
            printf("\n  Time: %lu ms", ingest_time);
            printf("\n  Rate: %lu logs/sec",
                   ingest_time > 0 ? (actual_ingested * 1000 / ingest_time) : 0);
            printf("\n  Ingested: %lu logs", actual_ingested);
            printf("\n  Skipped: %lu logs", actual_skipped);
            printf("\n  Queue: %lu files remaining / %lu total processed", files_remaining, consume_idx);
            printf("\n--- STATS BLOCK ---------------------------------------------------------------------------\n");

            // Check if simulator was suspended due to queue full
            if (produce_idx - consume_idx < MAX_RAW_FILES) {
                UINT thread_state;
                tx_thread_info_get(&simulator_thread, 0, &thread_state, 0, 0, 0, 0, 0, 0);
                if (thread_state == TX_SUSPENDED) {
                    tx_thread_resume(&simulator_thread);
                    printf("\nOK [INGESTION] Queue space available (%lu/%lu). Simulator Resumed.\n",
                           (produce_idx - consume_idx), MAX_RAW_FILES);
                }
            }
        }
        else {
            // ========================================================
            // FAILURE - Log error and skip file
            // ========================================================
            consume_idx++;

            uint32_t files_remaining = produce_idx - consume_idx;

            printf("\nERROR [INGESTION] Status: %d, batch_%lu.raw skipped (Queue: %lu remaining / %lu total processed)\n",
                   state, consume_idx - 1, files_remaining, consume_idx);

            if (db == nullptr) {
                printf("\n[INGESTION] Database closed due to recovery, will reopen next iteration\n");
            }

            tx_thread_sleep(100);
        }

//        tx_thread_sleep(1);
    }
}

//=======================================================================================
//
//=======================================================================================
UINT MPLIB_STORAGE::bindAndStep(const DS_LOG_STRUCT& log) {
    if (insert_stmt == nullptr) return SQLITE_ERROR;

    sqlite3_bind_int(insert_stmt, 1, log.log_index);

    // Use fixed lengths and SQLITE_STATIC to skip strlen() and internal copies
    sqlite3_bind_text(insert_stmt, 2, log.message, LOG_LENGTH, SQLITE_STATIC);
    sqlite3_bind_text(insert_stmt, 3, log.category, CAT_LENGTH, SQLITE_STATIC);

    sqlite3_bind_int(insert_stmt, 4, log.token);
    sqlite3_bind_int(insert_stmt, 5, log.local_log_index);
    sqlite3_bind_int(insert_stmt, 6, log.timestamp_at_store);
    sqlite3_bind_int(insert_stmt, 7, log.timestamp_at_log);
    sqlite3_bind_int(insert_stmt, 8, log.severity);

    int status = sqlite3_step(insert_stmt);

    sqlite3_reset(insert_stmt);
//    sqlite3_clear_bindings(insert_stmt);

    return status;
}

//=======================================================================================
//
//=======================================================================================
void MPLIB_STORAGE::captureLog(DS_LOG_STRUCT& log) {
    if (current_index >= LOGS_PER_BUFFER) {
        ULONG ready_flag = (active_fill_buffer == psram_buffer_A) ? FLAG_BUF_A_READY : FLAG_BUF_B_READY;
        ULONG next_free  = (ready_flag == FLAG_BUF_A_READY) ? FLAG_BUF_B_FREE : FLAG_BUF_A_FREE;

        // 1. SYNC: Finalize PSRAM writes before signaling
        __DSB();

        // 2. SIGNAL: Tell ingestor this buffer is full
        tx_event_flags_set(&staging_events, ready_flag, TX_OR);

        // 3. BACKPRESSURE: Block until the OTHER buffer is free
        //    The ingestor sets the FREE flag after COMMIT + flag clear.
        //    TX_AND_CLEAR consumes the free token so it's one-shot.
        ULONG actual_f;
        UINT status = tx_event_flags_get(&staging_events, next_free,
                                         TX_AND_CLEAR, &actual_f, TX_WAIT_FOREVER);

        if (status != TX_SUCCESS) {
            // Should never happen with TX_WAIT_FOREVER, but guard anyway
            printf("\nERROR [SIMULATOR] Backpressure wait failed (%u)\n", status);
        }

        // 4. SWAP: Switch to the now-free standby buffer
        active_fill_buffer = (active_fill_buffer == psram_buffer_A) ? psram_buffer_B : psram_buffer_A;
        current_index = 0;
    }

    log.local_log_index = current_index;
    memcpy((void*)&active_fill_buffer[current_index], &log, sizeof(DS_LOG_STRUCT));
    current_index++;
}

//=======================================================================================
//
//=======================================================================================
bool MPLIB_STORAGE::ingestRawToSQLite(const char* filename, UINT *status) {
    FX_FILE raw_file;
    ULONG bytes_read = 0;

    const uint32_t READ_CHUNK_SIZE = WRITE_CHUNK_SIZE; // 512 logs
    const uint32_t READ_BYTES = READ_CHUNK_SIZE * sizeof(DS_LOG_STRUCT);

    bool success = true;
    uint32_t total_logs_ingested = 0;
    uint32_t total_logs_skipped = 0;
    uint32_t chunk_count = 0;
    uint32_t successful_chunks = 0;


    // Open file
    if (fx_file_open(&sdio_disk, &raw_file, (CHAR*)filename, FX_OPEN_FOR_READ) != FX_SUCCESS) {
        printf("\nERROR [INGESTION] Failed to open %s\n", filename);
        return false;
    }
    printf("\nOK [INGESTION] Reading %s\n", filename);
    // ================================================================
    // Process file in chunks, one transaction per chunk
    // ================================================================
    while (total_logs_ingested < LOGS_PER_BUFFER) {
        // Invalidate cache for DMA safety
        SCB_InvalidateDCache_by_Addr((uint32_t *)sram_landing_zone, SRAM_LANDING_SIZE);

        // Read one chunk (512 logs = ~112KB)
        *status = fx_file_read(&raw_file, sram_landing_zone, READ_BYTES, &bytes_read);

        if (*status != FX_SUCCESS || bytes_read == 0) {
            if (bytes_read == 0 && total_logs_ingested > 0) {
                // End of file reached, this is OK
                printf("\nOK [INGESTION] End of file reached after %lu logs\n", total_logs_ingested);
                break;
            }
            printf("\nERROR [INGESTION] Read failed at chunk %lu, code: %d\n", chunk_count, *status);
            success = false;
            break;
        }

        DS_LOG_STRUCT* logs = (DS_LOG_STRUCT*)sram_landing_zone;
        uint32_t num_logs = bytes_read / sizeof(DS_LOG_STRUCT);

        // ============================================================
        // BEGIN TRANSACTION FOR THIS CHUNK ONLY
        // ============================================================
        *status = sqlite3_exec(db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
        if (*status != SQLITE_OK) {
            printf("\nERROR [INGESTION] BEGIN failed for chunk %lu: %s\n",
                   chunk_count, sqlite3_errmsg(db));
            success = false;
            break;
        }

//        printf("\nDEBUG [INGESTION] Processing chunk %lu (%lu logs)\n", chunk_count, num_logs);

        // Process all logs in this chunk
        uint32_t chunk_skipped = 0;
        for (uint32_t i = 0; i < num_logs; i++) {
            *status = this->bindAndStep(logs[i]);

            if (*status != SQLITE_DONE) {
                int err = sqlite3_errcode(db);

                if (*status == SQLITE_NOMEM || *status == 7) {
                	printf("\nCRITICAL [INGESTION] Out of memory detected in chunk %lu, releasing db memory !!!\n", chunk_count);
					// Force page cache release
					sqlite3_db_release_memory(db);

					// Retry once
					*status = this->bindAndStep(logs[i]);
					if (*status != SQLITE_DONE) {
						chunk_skipped++;
						total_logs_skipped++;
						continue;
					}
				}
                else {
                	printf("\nERROR [INGESTION] unmanaged error from bind and step: %d", err);
					// Non-critical errors - skip log and continue
					chunk_skipped++;
					total_logs_skipped++;
					continue;
                }

                // Critical errors - abort entire file
                if (err == SQLITE_CORRUPT || err == SQLITE_NOTADB || err == SQLITE_IOERR) {
                    printf("\nCRITICAL [INGESTION] Corruption detected in chunk %lu!\n", chunk_count);
                    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
                    fx_file_close(&raw_file);
                    this->recoverDatabase();
                    return false;
                }
            }
            else {

            }
        }

        // ============================================================
        // COMMIT THIS CHUNK
        // ============================================================
        *status = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        if (*status != SQLITE_OK) {
            int err = sqlite3_errcode(db);
            printf("\nERROR [INGESTION] COMMIT chunk %lu failed: %s (code: %d)\n",
                   chunk_count, sqlite3_errmsg(db), err);

            // Rollback this chunk
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);

            // Check for critical errors
            if (err == SQLITE_CORRUPT || err == SQLITE_NOTADB) {
                fx_file_close(&raw_file);
                this->recoverDatabase();
                return false;
            }

            // ============================================================
            // CHECKPOINT WAL (Flush to main database)
            // ============================================================
            if (chunk_count % 2 == 0) {
				int wal_log = 0, wal_ckpt = 0;
//				*status = sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, &wal_log, &wal_ckpt);
				*status = sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, &wal_log, &wal_ckpt);
				if (*status == SQLITE_OK) {
					printf("\nOK [INGESTION] WAL checkpoint: %d frames, %d checkpointed\n",
						   wal_log, wal_ckpt);
				} else {
					printf("\nWARN [INGESTION] WAL checkpoint returned: %d\n", *status);
				}
            }

            // Non-critical commit failure - continue to next chunk
            printf("\nWARN [INGESTION] Chunk %lu rolled back, continuing...\n", chunk_count);
        } else {
            // Success!
        	// Release page cache after every commit
			sqlite3_db_release_memory(db);
            successful_chunks++;
//            printf("\nOK [INGESTION] Chunk %lu committed (%lu logs, %lu skipped)\n", chunk_count, num_logs - chunk_skipped, chunk_skipped);
        }

        total_logs_ingested += num_logs;
        chunk_count++;

        // Progress indicator every 2 chunks
        if (chunk_count % 2 == 0) {
//            printf("\n#\n");
        }
    }

    fx_file_close(&raw_file);

    // ================================================================
    // Final Summary and Cleanup
    // ================================================================
    if (successful_chunks > 0) {
        printf("\n--- STATS BLOCK ---------------------------------------------------------------------------");
        printf("\nOK [INGESTION] File processing summary:");
        printf("\n  File: %s", filename);
        printf("\n  Total logs processed: %lu", total_logs_ingested);
        printf("\n  Logs ingested: %lu", total_logs_ingested - total_logs_skipped);
        printf("\n  Logs skipped: %lu", total_logs_skipped);
        printf("\n  Chunks committed: %lu / %lu", successful_chunks, chunk_count);
        printf("\n--- STATS BLOCK ---------------------------------------------------------------------------\n");

        // ========================================
        // UPDATE GLOBAL STATS
        // ========================================
        ing_total_logs += (total_logs_ingested - total_logs_skipped);  // Only valid logs
        ing_total_skipped += total_logs_skipped;  // Track skipped globally


        // Delete the raw file
        fx_media_flush(&sdio_disk);
        *status = fx_file_delete(&sdio_disk, (CHAR*)filename);
        if (*status == FX_SUCCESS) {
            printf("\nOK [INGESTION] Raw file %s deleted\n", filename);
        } else {
            printf("\nWARN [INGESTION] Failed to delete %s, code: 0x%02X\n", filename, *status);
        }

        return true;
    } else {
        // No chunks successfully committed
        printf("\nERROR [INGESTION] No chunks successfully committed for %s\n", filename);

        // Still delete the problematic file
        fx_media_flush(&sdio_disk);
        fx_file_delete(&sdio_disk, (CHAR*)filename);

        return false;
    }
}

//=======================================================================================
//
//=======================================================================================
UINT MPLIB_STORAGE::writeRawFile(const char* filename, volatile DS_LOG_STRUCT* buffer_in_psram, uint32_t actual_count) {
    FX_FILE raw_file;
    UINT status;

    const uint32_t CHUNK_SIZE = WRITE_CHUNK_SIZE; // 512 logs

    status = fx_file_create(&sdio_disk, (CHAR*)filename);
    if (status != FX_SUCCESS && status != FX_ALREADY_CREATED) {
        printf("ERROR [STORAGE] Create Fail: 0x%02X\n", status);
        return status;
    }

    status = fx_file_open(&sdio_disk, &raw_file, (CHAR*)filename, FX_OPEN_FOR_WRITE);
    if (status != FX_SUCCESS) {
        printf("ERROR [STORAGE] Open Fail: 0x%02X\n", status);
        return status;
    }

    uint32_t logs_rem = LOGS_PER_BUFFER;

    uint32_t offset = 0;
    uint32_t dma_start_time = tx_time_get();

    // Single memory barrier before all DMA operations
    __DSB();

    while (logs_rem > 0) {
        uint32_t count = (logs_rem >= CHUNK_SIZE) ? CHUNK_SIZE : logs_rem;
        uint32_t sz = count * sizeof(DS_LOG_STRUCT);

        // ============================================================
        // DMA TRANSFER: PSRAM â†’ AXI SRAM (Hardware accelerated!)
        // ============================================================

        HAL_StatusTypeDef dma_status = HAL_DMA_Start(
            &hdma_mem2mem,
            (uint32_t)&buffer_in_psram[offset],  // Source: PSRAM address
            (uint32_t)sram_landing_zone,         // Dest: Landing zone
            sz                                   // Size in BYTES
        );

        if (dma_status != HAL_OK) {
            printf("\nERROR [STORAGE] DMA Start failed: %d\n", dma_status);
            // Fallback to memcpy
            memcpy(sram_landing_zone, (const void*)&buffer_in_psram[offset], sz);
        } else {
            // Wait for DMA to complete (polling)
            dma_status = HAL_DMA_PollForTransfer(
                &hdma_mem2mem,
                HAL_DMA_FULL_TRANSFER,  // Wait for complete transfer
                1000                     // Timeout: 1000ms
            );

            if (dma_status != HAL_OK) {
                printf("\nERROR [STORAGE] DMA Poll timeout: %d\n", dma_status);
                // Fallback to memcpy for remaining data
                memcpy(sram_landing_zone, (const void*)&buffer_in_psram[offset], sz);
            }
        }

        // Ensure DMA transfer is visible before SD write
        __DSB();

        // Write to SD card (FileX with IDMA handles this)
        tx_mutex_get(&sd_io_mutex, TX_WAIT_FOREVER);

        status = fx_file_write(&raw_file, sram_landing_zone, sz);
        if (status != FX_SUCCESS) {
            printf("\nERROR [STORAGE] Write Fail: 0x%02X at offset %lu\n", status, offset);
            tx_mutex_put(&sd_io_mutex);
            break;
        }

        tx_mutex_put(&sd_io_mutex);

        logs_rem -= count;
        offset += count;
    }

    uint32_t dma_total_time = tx_time_get() - dma_start_time;

    // Single flush at end
    fx_media_flush(&sdio_disk);
    fx_file_close(&raw_file);

    printf("\nOK [STORAGE] DMA write complete: %s (%lu ms total, %lu logs/sec)\n",
           filename, dma_total_time,
           dma_total_time > 0 ? (LOGS_PER_BUFFER * 1000 / dma_total_time) : 0);

    return status;
}

//=======================================================================================
//
//=======================================================================================
bool MPLIB_STORAGE::createTable() {
    char *zErrMsg = 0;
    const char* sql_create =
    "CREATE TABLE IF NOT EXISTS ds_logs ("
    "log_index INTEGER PRIMARY KEY, "
    "message TEXT NOT NULL, "
    "category TEXT, "
    "token INTEGER, "
    "local_log_index INTEGER, "
    "timestamp_at_store INTEGER, "
    "timestamp_at_log INTEGER, "
    "severity INTEGER"
    ");";

    int status = sqlite3_exec(db, sql_create, NULL, NULL, &zErrMsg);
    if (status == SQLITE_OK) {
        // REMOVED: wal_checkpoint (Not valid for DELETE journal mode)
//        tx_event_flags_set(&db_flags, 0x08, TX_OR);
        printf("\nOK TO CREATE TABLE\n");
    } else {
        printf("\nERROR TO CREATE TABLE: %d, message: %s !!!\n", status, zErrMsg);
        sqlite3_free(zErrMsg);
        return false;
    }

    // INDEX DEFERRED: idx_logs_category removed during bulk ingestion for throughput.
    // Create it post-load with: CREATE INDEX idx_logs_category ON ds_logs(category);

    return true;
}

//=======================================================================================
//
//=======================================================================================
void MPLIB_STORAGE::recoverDatabase() {
    printf("\n--- STATS BLOCK ---------------------------------------------------------------------------");
    printf("\n[RECOVERY] Database corruption detected!");
    printf("\n[RECOVERY] Initiating recovery sequence...");
    printf("\n--- STATS BLOCK ---------------------------------------------------------------------------\n");

    // ================================================================
    // STEP 1: Finalize prepared statement
    // ================================================================
    if (insert_stmt != nullptr) {
        int rc = sqlite3_finalize(insert_stmt);
        if (rc == SQLITE_OK) {
            printf("\n[RECOVERY] Statement finalized successfully.");
        } else {
            printf("\n[RECOVERY] Statement finalize error: %d (%s)", rc, sqlite3_errstr(rc));
        }
        insert_stmt = nullptr;
    } else {
        printf("\n[RECOVERY] No statement to finalize (already null).");
    }

    // ================================================================
    // STEP 2: Close database handle
    // ================================================================
    if (db != nullptr) {
        // Use sqlite3_close_v2 - safer for embedded, handles busy connections
        int rc = sqlite3_close_v2(db);
        if (rc == SQLITE_OK) {
            printf("\n[RECOVERY] Database handle closed successfully.");
        } else if (rc == SQLITE_BUSY) {
            printf("\n[RECOVERY] DB still busy, forcing close...");
            // Try harder - interrupt any pending operations
            sqlite3_interrupt(db);
            tx_thread_sleep(10);  // Give it a moment
            rc = sqlite3_close_v2(db);
            if (rc == SQLITE_OK) {
                printf("\n[RECOVERY] Database handle closed after interrupt.");
            } else {
                printf("\n[RECOVERY] DB Close Error: %d (%s) - forcing pointer null",
                       rc, sqlite3_errstr(rc));
            }
        } else {
            printf("\n[RECOVERY] DB Close Error: %d (%s) - forcing pointer null",
                   rc, sqlite3_errstr(rc));
        }
        db = nullptr;
    } else {
        printf("\n[RECOVERY] No database handle to close (already null).");
    }

    // ================================================================
    // STEP 3: Flush and close any pending FileX operations
    // ================================================================
    printf("\n[RECOVERY] Flushing SD card media...");
    UINT fx_status = fx_media_flush(&sdio_disk);
    if (fx_status == FX_SUCCESS) {
        printf("\n[RECOVERY] Media flushed successfully.");
    } else {
        printf("\n[RECOVERY] Media flush error: 0x%02X", fx_status);
    }

    // ================================================================
    // STEP 4: Delete corrupted database files
    // ================================================================
    printf("\n[RECOVERY] Deleting corrupted database files...");

    UINT status = delete_database_files();
    if (status == FX_SUCCESS) {
        printf("\n[RECOVERY] Database files deleted successfully.");
    } else {
        printf("\n[RECOVERY] Database file deletion returned: 0x%02X", status);
    }

    // Also delete WAL and SHM files if they exist
    fx_file_delete(&sdio_disk, "logs.db-wal");
    fx_file_delete(&sdio_disk, "logs.db-shm");
    printf("\n[RECOVERY] WAL/SHM files cleaned up.");

    // ================================================================
    // STEP 5: Recreate database structure (without keeping it open)
    // ================================================================
    printf("\n[RECOVERY] Recreating database structure...");

    // Temporarily open database to recreate structure
    int rc = sqlite3_open(DB_NAME, &db);
    if (rc != SQLITE_OK) {
        printf("\nERROR [RECOVERY] Failed to reopen DB: %s", sqlite3_errmsg(db));
        printf("\nFAIL [RECOVERY] System could not be restored!");
        db = nullptr;
        return;
    }

    // Apply configuration
    tuneDbConfig();
    printf("\n[RECOVERY] Database configuration applied.");

    // Recreate table structure
    if (!createTable()) {
        printf("\nERROR [RECOVERY] Failed to recreate table!");
        sqlite3_close_v2(db);
        db = nullptr;
        printf("\nFAIL [RECOVERY] System could not be restored!");
        return;
    }

    printf("\n[RECOVERY] Table structure recreated successfully.");

    // ================================================================
    // STEP 6: Close database (ingestion thread will reopen)
    // ================================================================
    rc = sqlite3_close_v2(db);
    if (rc == SQLITE_OK) {
        printf("\n[RECOVERY] Database closed (ready for ingestion thread to reopen).");
    } else {
        printf("\n[RECOVERY] Warning: DB close returned %d", rc);
    }
    db = nullptr;

    // ================================================================
    // STEP 7: Reset counters and state
    // ================================================================
    // Don't reset produce_idx/consume_idx - let the pipeline continue
    // Just reset the statement pointer
    insert_stmt = nullptr;

    printf("\n--- STATS BLOCK ---------------------------------------------------------------------------");
    printf("\n[RECOVERY] Recovery complete!");
    printf("\n[RECOVERY] Ingestion thread will reopen DB on next file.");
    printf("\n--- STATS BLOCK ---------------------------------------------------------------------------\n");

    // Small delay to let things settle
    tx_thread_sleep(100);
}

//=======================================================================================
//
//=======================================================================================
void print_log( DS_LOG_STRUCT* log) {
//	printf("\nLOG: %lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%s\t%s\n", log->log_index, log->local_log_index, log->timestamp_at_store, log->timestamp_at_log, log->severity, log->token, log->category, log->message );
	printf("\nLOG: %lu\t%lu\t%lu\t%lu\t%lu\t%lu\n", log->log_index, log->local_log_index, log->timestamp_at_store, log->timestamp_at_log, log->severity, log->token);
}

//=======================================================================================
//
//=======================================================================================
bool MPLIB_STORAGE::verifyLayout() {
    uint32_t logs_space = (uint32_t)&__psram_logs_end - (uint32_t)&__psram_logs_start;
    uint32_t required = sizeof(psram_buffer_A) + sizeof(psram_buffer_B);

    if (required > logs_space) {
        printf("ERROR [STORAGE] PSRAM Logging Section too small!\n");
        return false;
    }

    return true;
}

//=======================================================================================
//
//=======================================================================================
UINT MPLIB_STORAGE::delete_database_files()
{
	UINT status;

//	const char *file_pattern = "batch_";
//	const char *file_extension = ".raw";
//	char file_name[15];
//
//	for(uint8_t i=0; i<MAX_RAW_FILES; i++) {
//		memset(file_name,0,15);
//		snprintf(file_name,15,"%s%d%s", file_pattern, i, file_extension);
//		printf("\nOK [STORAGE] deleting raw files if present: %s", file_name);
//		status = fx_file_delete(&sdio_disk, file_name);
//	}

//	status = fx_file_delete(&sdio_disk, "batch_0.raw");
//	status = fx_file_delete(&sdio_disk, "batch_1.raw");
//	status = fx_file_delete(&sdio_disk, "batch_2.raw");
//	status = fx_file_delete(&sdio_disk, "batch_3.raw");
//	status = fx_file_delete(&sdio_disk, "batch_4.raw");
//	status = fx_file_delete(&sdio_disk, "batch_4.raw");

    status = fx_file_delete(&sdio_disk, (CHAR*)DB_NAME);
    if (status == FX_SUCCESS) {
        printf("\nOK [STORAGE] Old database deleted for fresh start\n");
    } else if (status != FX_NOT_FOUND) {
        printf("\nWARNING [STORAGE] Delete failed (Status: 0x%02X). Handle might be busy!\n", status);
    }
	else if (status == FX_WRITE_PROTECT)
	{
		printf("\nERROR [STORAGE] database log file is write protected, cannot delete automagically\n");
	}

	status = fx_file_delete(&sdio_disk, (CHAR*)"logs.db-journal");
	if (status == FX_SUCCESS) {
	    printf("\nOK [STORAGE] journal file deleted\n");
	} else if (status == FX_NOT_FOUND) {
	    // This is the normal case if the previous run closed cleanly
	}

	fx_media_flush(&sdio_disk);

	return status;
}

//=======================================================================================
//
//=======================================================================================

// DMA Transfer Complete Callback (called by interrupt)
void HAL_DMA_XferCpltCallback(DMA_HandleTypeDef *hdma)
{
    if (hdma == &(hdma_mem2mem)) {
        // Signal completion via semaphore
        tx_semaphore_put(&(dma_complete_sem));
        dma_transfer_complete = true;
    }
}

//=======================================================================================
//
//=======================================================================================

// DMA Error Callback
void HAL_DMA_XferErrorCallback(DMA_HandleTypeDef *hdma)
{
    if (hdma == &(hdma_mem2mem)) {
        printf("\nERROR [DMA] Transfer error!\n");
        // Signal error - still release semaphore but set error flag
        dma_transfer_complete = false;
        tx_semaphore_put(&(dma_complete_sem));
    }
}

//=======================================================================================
//
//=======================================================================================

// Interrupt-based writeRawFile
UINT MPLIB_STORAGE::writeRawFile_interrupt(const char* filename, volatile DS_LOG_STRUCT* buffer_in_psram, uint32_t actual_count) {
    FX_FILE raw_file;
    UINT status;

    const uint32_t CHUNK_SIZE = WRITE_CHUNK_SIZE;

    status = fx_file_create(&sdio_disk, (CHAR*)filename);
    if (status != FX_SUCCESS && status != FX_ALREADY_CREATED) {
        printf("ERROR [STORAGE] Create Fail: 0x%02X\n", status);
        return status;
    }

    status = fx_file_open(&sdio_disk, &raw_file, (CHAR*)filename, FX_OPEN_FOR_WRITE);
    if (status != FX_SUCCESS) {
        printf("ERROR [STORAGE] Open Fail: 0x%02X\n", status);
        return status;
    }

    uint32_t logs_rem = LOGS_PER_BUFFER;

    uint32_t offset = 0;

    __DSB();

    while (logs_rem > 0) {
        uint32_t count = (logs_rem >= CHUNK_SIZE) ? CHUNK_SIZE : logs_rem;
        uint32_t sz = count * sizeof(DS_LOG_STRUCT);

        // ============================================================
        // DMA TRANSFER WITH INTERRUPT (Non-blocking!)
        // ============================================================

        dma_transfer_complete = false;

        HAL_StatusTypeDef dma_status = HAL_DMA_Start_IT(
            &hdma_mem2mem,
            (uint32_t)&buffer_in_psram[offset],
            (uint32_t)sram_landing_zone,
            sz
        );

        if (dma_status != HAL_OK) {
            printf("\nERROR [STORAGE] DMA Start_IT failed: %d\n", dma_status);
            memcpy(sram_landing_zone, (const void*)&buffer_in_psram[offset], sz);
        } else {
            // Wait for DMA completion via semaphore (released by interrupt)
            UINT sem_status = tx_semaphore_get(&dma_complete_sem, 1000); // 1 second timeout

            if (sem_status != TX_SUCCESS || !dma_transfer_complete) {
                printf("\nERROR [STORAGE] DMA timeout or error\n");
                HAL_DMA_Abort(&hdma_mem2mem);
                memcpy(sram_landing_zone, (const void*)&buffer_in_psram[offset], sz);
            }
        }

        __DSB();

        // Write to SD (while CPU was waiting, DMA was working!)
        tx_mutex_get(&sd_io_mutex, TX_WAIT_FOREVER);
        status = fx_file_write(&raw_file, sram_landing_zone, sz);
        tx_mutex_put(&sd_io_mutex);

        if (status != FX_SUCCESS) {
            printf("\nERROR [STORAGE] Write Fail: 0x%02X\n", status);
            break;
        }

        logs_rem -= count;
        offset += count;
    }

    fx_media_flush(&sdio_disk);
    fx_file_close(&raw_file);

    return status;
}

//=======================================================================================
// INGESTOR_DIRECT - Direct PSRAM to SQLite (bypasses raw files)
//=======================================================================================
void MPLIB_STORAGE::ingestor_direct(ULONG thread_input) {
    const char *sql = "INSERT INTO ds_logs (log_index, message, category, token, local_log_index, timestamp_at_store, timestamp_at_log, severity) VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    uint32_t buffer_counter = 0;
    int rc;

    while(1) {
        ULONG actual_flags;
        // Wait for either buffer READY flag (0x01 | 0x02)
        if (tx_event_flags_get(&staging_events, FLAG_BUF_A_READY | FLAG_BUF_B_READY,
                               TX_OR, &actual_flags, TX_WAIT_FOREVER) != TX_SUCCESS) continue;

        // Reopen handle if needed
        if (db == nullptr) {
            if (sqlite3_open(DB_NAME, &db) != SQLITE_OK) {
                printf("\nERROR [INGEST] Failed to open DB: %s\n", sqlite3_errmsg(db));
                tx_thread_sleep(1000);
                continue;
            }
            tuneDbConfig();
            rc = sqlite3_prepare_v2(db, sql, -1, &insert_stmt, nullptr);
            if (rc != SQLITE_OK) {
                printf("\nERROR [INGEST] Prepare failed: %s\n", sqlite3_errmsg(db));
                sqlite3_close_v2(db);
                db = nullptr;
                tx_thread_sleep(1000);
                continue;
            }
        }

        // Determine which buffer to consume
        ULONG ready_bit = (actual_flags & FLAG_BUF_A_READY) ? FLAG_BUF_A_READY : FLAG_BUF_B_READY;
        ULONG free_bit  = (ready_bit == FLAG_BUF_A_READY)   ? FLAG_BUF_A_FREE  : FLAG_BUF_B_FREE;
        volatile DS_LOG_STRUCT* src_buffer = (ready_bit == FLAG_BUF_A_READY) ? psram_buffer_A : psram_buffer_B;

        SCB_InvalidateDCache_by_Addr((uint32_t*)src_buffer, LOGS_PER_BUFFER * sizeof(DS_LOG_STRUCT));

        uint32_t start_time = tx_time_get();

        // --- SINGLE TRANSACTION PER BUFFER ---
        rc = sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            printf("\nERROR [INGEST] BEGIN failed: %s\n", sqlite3_errmsg(db));
            // Release: clear READY, set FREE so simulator isn't stuck forever
            tx_event_flags_set(&staging_events, ~ready_bit, TX_AND);
            tx_event_flags_set(&staging_events, free_bit, TX_OR);
            continue;
        }

        bool batch_ok = true;
        for (uint32_t i = 0; i < LOGS_PER_BUFFER; i++) {
            int step_rc = this->bindAndStep((const DS_LOG_STRUCT&)src_buffer[i]);
            if (step_rc != SQLITE_DONE) {
                printf("\nERROR [INGEST] Insert %lu failed: %d (%s)\n",
                       i, step_rc, sqlite3_errmsg(db));
                batch_ok = false;
                break;
            }
        }

        if (batch_ok) {
            rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
            if (rc != SQLITE_OK) {
                printf("\nERROR [INGEST] COMMIT failed: %s\n", sqlite3_errmsg(db));
                sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            }
        } else {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        }

        // Update stats
        uint32_t elapsed = tx_time_get() - start_time;
        ing_total_logs += LOGS_PER_BUFFER;
        ing_last_time = tx_time_get();

        // Release buffer: clear READY bit, then signal FREE to unblock simulator
        tx_event_flags_set(&staging_events, ~ready_bit, TX_AND);
        tx_event_flags_set(&staging_events, free_bit, TX_OR);

        printf("\n>> [INGEST] Buffer %s Done | %lu ms | Rate: %lu l/s\n",
               (ready_bit == FLAG_BUF_A_READY ? "A" : "B"), elapsed,
               (LOGS_PER_BUFFER * 1000 / (elapsed > 0 ? elapsed : 1)));

        buffer_counter++;
        if (buffer_counter % 5 == 0) {
            sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
        }
    }
}

