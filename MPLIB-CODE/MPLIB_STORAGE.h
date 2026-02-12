/*
 * MPLIB_STORAGE.h
 *
 *  Created on: Feb 4, 2026
 *      Author: Packet
 *  Optimized: Feb 10, 2026
 */
#ifndef MPLIB_STORAGE_H_
#define MPLIB_STORAGE_H_


#include "stdio.h"
#include "stdint.h"
#include "fx_api.h"
#include "sqlite3.h"


//=======================================================================================
// CONFIGURATION
//=======================================================================================

#define CAT_LENGTH 24
#define LOG_LENGTH 160

//=======================================================================================
// MEMORY & THREAD CONFIGURATION
//=======================================================================================
#define SRAM_LANDING_SIZE			128*1024
#define SQLITE_STACK_SIZE			64*1024
#define SIMULATOR_STACK_SIZE		4*1024
#define INGESTION_STACK_SIZE		80*1024
#define STORAGE_STACK_SIZE			12*1024

#define LOGS_PER_BUFFER 16384	// 16384 logs × 224B = ~3.6MB per buffer (×2 = 7.2MB of 32MB PSRAM)

// Event flag bits for double-buffer synchronization
//   0x01 = Buffer A ready (full, waiting for ingestor)
//   0x02 = Buffer B ready (full, waiting for ingestor)
//   0x04 = Buffer A free  (ingestor done, simulator may write)
//   0x08 = Buffer B free  (ingestor done, simulator may write)
#define FLAG_BUF_A_READY  0x01
#define FLAG_BUF_B_READY  0x02
#define FLAG_BUF_A_FREE   0x04
#define FLAG_BUF_B_FREE   0x08

// OPTIMIZATION: Larger chunk size for fewer mutex cycles
#define WRITE_CHUNK_SIZE 512  // 512 logs × 224B = 114,688 bytes (~112KB)

// perfectly aligned 224-byte struct
typedef struct __attribute__((packed, aligned(32))) {
    uint32_t log_index;
    uint32_t token;
    uint32_t local_log_index;
    uint32_t timestamp_at_store;
    uint32_t timestamp_at_log;
    uint32_t severity;
    char category[24];
    char message[160];
    uint8_t reserved[16];
} DS_LOG_STRUCT, *DS_LOG_STRUCT_PTR;


//=======================================================================================
// C THREAD ENTRY POINTS
//=======================================================================================

#ifdef __cplusplus
extern "C" {
#endif

void StartStorageServices(unsigned long thread_input);

void simulator_thread_entry(ULONG instance_ptr);

void ingestion_thread_entry(ULONG thread_input);

void ingestion_direct_thread_entry(ULONG thread_input);

#ifdef __cplusplus
}
#endif


//=======================================================================================
// MPLIB_STORAGE CLASS
//=======================================================================================

#ifdef __cplusplus

class MPLIB_STORAGE {
	static int iSTORAGE;
	static MPLIB_STORAGE *instance;
	static char *name_singleton;
public:
	static MPLIB_STORAGE* CreateInstance() {
		if(iSTORAGE==0) {
			instance =new MPLIB_STORAGE;
			snprintf(name_singleton, CAT_LENGTH, "STORAGE");
			iSTORAGE=1;
		}

		return instance;
	}

    bool init();

    void simulator();

    void work();

    void ingestor(ULONG thread_input);

    void ingestor_direct(ULONG thread_input);

	void setStart(bool value);

protected:
	void init_psram();

    void captureLog(DS_LOG_STRUCT& log);

    UINT writeRawFile(const char* filename, volatile DS_LOG_STRUCT* buffer, uint32_t actual_count);

    UINT writeRawFile_interrupt(const char* filename, volatile DS_LOG_STRUCT* buffer_in_psram, uint32_t actual_count);

    bool ingestRawToSQLite(const char* filename, UINT *status);

    UINT bindAndStep(const DS_LOG_STRUCT& log);

private:
    bool started = false;
    sqlite3* db = nullptr;
    sqlite3_stmt* insert_stmt = nullptr;

    bool createTable();
    void recoverDatabase();
    void tuneDbConfig();

    UINT delete_database_files();
    bool verifyLayout();

    // Pointers to the PSRAM sections defined in your linker script
    volatile DS_LOG_STRUCT* buffer_A;
    volatile DS_LOG_STRUCT* buffer_B;
    volatile DS_LOG_STRUCT* active_fill_buffer;

    uint32_t current_index = 0;

    uint32_t buffer_A_count = 0;
    uint32_t buffer_B_count = 0;
};

//=======================================================================================
// GLOBAL INSTANCE
//=======================================================================================
extern MPLIB_STORAGE *STORAGE;

#endif
#endif /* MPLIB_STORAGE_H_ */
