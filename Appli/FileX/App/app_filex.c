/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_filex.c
  * @author  MCD Application Team
  * @brief   FileX applicative file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "app_filex.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "MPLIB_STORAGE.h"

#include <stdio.h>
#include "sqlite3.h"
#include "sqlite3_azure.h"
#include "main.h"


extern TX_THREAD storage_thread;
extern uint8_t storage_stack[STORAGE_STACK_SIZE];



/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* Main thread stack size */
#define FX_APP_THREAD_STACK_SIZE         12288
/* Main thread priority */
#define FX_APP_THREAD_PRIO               10
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* Main thread global data structures.  */
TX_THREAD       fx_app_thread;

/* Buffer for FileX FX_MEDIA sector cache. */
ALIGN_32BYTES (uint32_t fx_sd_media_memory[FX_STM32_SD_DEFAULT_SECTOR_SIZE / sizeof(uint32_t)]);
/* Define FileX global data structures.  */
FX_MEDIA        sdio_disk;

/* USER CODE BEGIN PV */
ULONG free_bytes;
FX_MEDIA* sqlite3_media_ptr = &sdio_disk;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/

/* Main thread entry function.  */
void fx_app_thread_entry(ULONG thread_input);

/* USER CODE BEGIN PFP */
static int randomness(void)
{
	int rnd;
	HAL_RNG_GenerateRandomNumber(&hrng, (uint32_t*)&rnd);
	return rnd;
}

//static int sql_callback(void*, int count, char** values, char** names)
//{
//    for(unsigned i = 0; i < count; i++)
//    	printf("\t%s\t%s", names[i], values[i]);
//
//    printf("\r\n");
//	return 0;
//}

sqlite3_int64 datetime(void){
	RTC_TimeTypeDef time;
	RTC_DateTypeDef date;

	HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BIN);
	HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN);

	sqlite3_int64 a = (14 - date.Month) / 12;
	sqlite3_int64 y = date.Year + 4800 - a;
	sqlite3_int64 m = date.Month + 12 * a - 3;

	sqlite3_int64 result = (date.Date + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045) * 86400000U;
	result += (time.Hours - 12) * 3600000 + time.Minutes * 60000 + time.Seconds * 1000 + time.SubSeconds * 1000 / (time.SecondFraction + 1);

	return result;
}
/* USER CODE END PFP */

/**
  * @brief  Application FileX Initialization.
  * @param memory_ptr: memory pointer
  * @retval int
*/
UINT MX_FileX_Init(VOID *memory_ptr)
{
  UINT ret = FX_SUCCESS;
  TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL*)memory_ptr;
  VOID *pointer;

/* USER CODE BEGIN MX_FileX_MEM_POOL */

/* USER CODE END MX_FileX_MEM_POOL */

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*Allocate memory for the main thread's stack*/
  ret = tx_byte_allocate(byte_pool, &pointer, FX_APP_THREAD_STACK_SIZE, TX_NO_WAIT);

/* Check FX_APP_THREAD_STACK_SIZE allocation*/
  if (ret != FX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }

/* Create the main thread.  */
  ret = tx_thread_create(&fx_app_thread, FX_APP_THREAD_NAME, fx_app_thread_entry, 0, pointer, FX_APP_THREAD_STACK_SIZE,
                         FX_APP_THREAD_PRIO, FX_APP_PREEMPTION_THRESHOLD, FX_APP_THREAD_TIME_SLICE, FX_APP_THREAD_AUTO_START);

/* Check main thread creation */
  if (ret != FX_SUCCESS)
  {
    return TX_THREAD_ERROR;
  }

/* USER CODE BEGIN MX_FileX_Init */

/* USER CODE END MX_FileX_Init */

/* Initialize FileX.  */
  fx_system_initialize();

/* USER CODE BEGIN MX_FileX_Init 1*/

/* USER CODE END MX_FileX_Init 1*/

  return ret;
}

/**
 * @brief  Main thread entry.
 * @param thread_input: ULONG user argument used by the thread entry
 * @retval none
*/
 void fx_app_thread_entry(ULONG thread_input)
 {

  UINT sd_status = FX_SUCCESS;

/* USER CODE BEGIN fx_app_thread_entry 0*/
  UINT tx_status = TX_SUCCESS;

  _Alignas(32) static uint64_t fx_sd_media_memory[1024]; // redefine here as it is generated not in a user section


//  MPLIB_STORAGE_PTR storage_handle = Get_Storage_Instance();
/* USER CODE END fx_app_thread_entry 0*/

/* Open the SD disk driver */
  sd_status =  fx_media_open(&sdio_disk, FX_SD_VOLUME_NAME, fx_stm32_sd_driver, (VOID *)FX_NULL, (VOID *) fx_sd_media_memory, sizeof(fx_sd_media_memory));

/* Check the media open sd_status */
  if (sd_status != FX_SUCCESS)
  {
     /* USER CODE BEGIN SD open error */
    printf("\nERROR TO OPEN FX MEDIA\n");
    /* USER CODE END SD open error */
  }

/* USER CODE BEGIN fx_app_thread_entry 1*/
  if (sd_status == FX_SUCCESS)
  {
	  printf("\nOK Fx media successfully opened.\n");

	//  fx_media_space_available(&sdio_disk, &free_bytes);

	//ULONG errors;
	//sd_status = fx_media_check(&sdio_disk, sqlite_heap, sizeof(sqlite_heap), (ULONG)7, &errors);

	sqlite3_azure_init(&sdio_disk, datetime, randomness);

	  tx_status = tx_thread_create(
		&storage_thread,
		"STORAGE",
		StartStorageServices,
		0,
		(VOID*)storage_stack,
		STORAGE_STACK_SIZE,
		10, 10, 0, 0
	  );
	  if (tx_status != TX_SUCCESS)
	  {
		  printf("ERROR TO START SQLITE DB THREAD: %d\n", tx_status);
	  }

	  tx_thread_resume(&storage_thread);
  }

/* USER CODE END fx_app_thread_entry 1*/
  }

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
