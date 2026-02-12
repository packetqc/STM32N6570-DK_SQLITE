/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    mce.c
  * @brief   This file provides code for the configuration
  *          of the MCE instances.
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
#include "mce.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

MCE_HandleTypeDef hmce1;
__ALIGN_BEGIN static const uint32_t NonceMCE1[2] __ALIGN_END = {
                            0x00000000,0x00000000};
__ALIGN_BEGIN static const uint32_t pKeyMCE1[4] __ALIGN_END = {
                            0x00000000,0x00000000,0x00000000,0x00000000};

/* MCE1 init function */
void MX_MCE1_Init(void)
{

  /* USER CODE BEGIN MCE1_Init 0 */

  /* USER CODE END MCE1_Init 0 */

  MCE_AESConfigTypeDef ContextAESConfig = {0};

  /* USER CODE BEGIN MCE1_Init 1 */

  /* USER CODE END MCE1_Init 1 */
  hmce1.Instance = MCE1;
  if (HAL_MCE_Init(&hmce1) != HAL_OK)
  {
    Error_Handler();
  }
  ContextAESConfig.Nonce[0]= NonceMCE1[0];
  ContextAESConfig.Nonce[1]= NonceMCE1[1];
  ContextAESConfig.Version = 0x0000;
  ContextAESConfig.pKey = (uint32_t *)pKeyMCE1;
  if (HAL_MCE_ConfigAESContext(&hmce1, &ContextAESConfig, MCE_NO_CONTEXT) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_MCE_EnableAESContext(&hmce1, MCE_NO_CONTEXT) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_MCE_SetRegionAESContext(&hmce1, MCE_NO_CONTEXT, MCE_REGION1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN MCE1_Init 2 */

  /* USER CODE END MCE1_Init 2 */

}

void HAL_MCE_MspInit(MCE_HandleTypeDef* mceHandle)
{

  if(mceHandle->Instance==MCE1)
  {
  /* USER CODE BEGIN MCE1_MspInit 0 */

  /* USER CODE END MCE1_MspInit 0 */
    /* MCE1 clock enable */
    __HAL_RCC_MCE1_CLK_ENABLE();
  /* USER CODE BEGIN MCE1_MspInit 1 */

  /* USER CODE END MCE1_MspInit 1 */
  }
}

void HAL_MCE_MspDeInit(MCE_HandleTypeDef* mceHandle)
{

  if(mceHandle->Instance==MCE1)
  {
  /* USER CODE BEGIN MCE1_MspDeInit 0 */

  /* USER CODE END MCE1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_MCE1_CLK_DISABLE();
  /* USER CODE BEGIN MCE1_MspDeInit 1 */

  /* USER CODE END MCE1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
