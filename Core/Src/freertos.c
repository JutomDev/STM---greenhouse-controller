/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "greenhouse_logic.h"
#include "greenhouse_hal_wrapper.h"
#include "usart.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
/* Static allocation control blocks and stacks for FreeRTOS objects to fit in the 4KB RAM */

/* Definitions for readTask (Static)
   Stack size reduced to 48 words (192 bytes) to meet strict RAM limit (<95%) */
osThreadId_t readTaskHandle;
static uint32_t readTaskStack[48];
static StaticTask_t readTaskControlBlock;
const osThreadAttr_t readTask_attributes = {
  .name = "readTask",
  .cb_mem = &readTaskControlBlock,
  .cb_size = sizeof(readTaskControlBlock),
  .stack_mem = &readTaskStack[0],
  .stack_size = sizeof(readTaskStack),
  .priority = (osPriority_t) osPriorityNormal,
};

/* Definitions for greenhouseQueue (Static)
   Length reduced to 2 elements to minimize static memory consumption. */
osMessageQueueId_t greenhouseQueueHandle;
static uint8_t greenhouseQueueBuffer[2 * sizeof(Greenhouse_Sensors_t)];
static StaticQueue_t greenhouseQueueControlBlock;
const osMessageQueueAttr_t greenhouseQueue_attributes = {
  .name = "greenhouseQueue",
  .cb_mem = &greenhouseQueueControlBlock,
  .cb_size = sizeof(greenhouseQueueControlBlock),
  .mq_mem = &greenhouseQueueBuffer[0],
  .mq_size = sizeof(greenhouseQueueBuffer),
};

/* Definitions for manualWateringTimer (Static) */
osTimerId_t manualWateringTimerHandle;
static StaticTimer_t manualWateringTimerControlBlock;
const osTimerAttr_t manualWateringTimer_attributes = {
  .name = "manualWateringTimer",
  .cb_mem = &manualWateringTimerControlBlock,
  .cb_size = sizeof(manualWateringTimerControlBlock),
};

/* Definitions for ledTimer (Static)
   Timer runs periodically every 100ms to drive status indicator LED */
osTimerId_t ledTimerHandle;
static StaticTimer_t ledTimerControlBlock;
const osTimerAttr_t ledTimer_attributes = {
  .name = "ledTimer",
  .cb_mem = &ledTimerControlBlock,
  .cb_size = sizeof(ledTimerControlBlock),
};

/* Definitions for uartMutex (Static) */
osMutexId_t uartMutexHandle;
static StaticSemaphore_t uartMutexControlBlock;
const osMutexAttr_t uartMutex_attributes = {
  .name = "uartMutex",
  .cb_mem = &uartMutexControlBlock,
  .cb_size = sizeof(uartMutexControlBlock),
};

/* Definitions for buttonSemaphore (Static) */
osSemaphoreId_t buttonSemaphoreHandle;
static StaticSemaphore_t buttonSemaphoreControlBlock;
const osSemaphoreAttr_t buttonSemaphore_attributes = {
  .name = "buttonSemaphore",
  .cb_mem = &buttonSemaphoreControlBlock,
  .cb_size = sizeof(buttonSemaphoreControlBlock),
};
/* Static allocation variables for defaultTask */
static uint32_t defaultTaskStack[100];
static StaticTask_t defaultTaskControlBlock;
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .cb_mem = &defaultTaskControlBlock,
  .cb_size = sizeof(defaultTaskControlBlock),
  .stack_mem = &defaultTaskStack[0],
  .stack_size = sizeof(defaultTaskStack),
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* Create static Mutex to protect UART serial communications */
  uartMutexHandle = osMutexNew(&uartMutex_attributes);
  if (uartMutexHandle == NULL) {
      /* Fallback logging will be used, which is safe */
  }
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* Create static Binary Semaphore for EXTI button interrupt synchronization.
     Initialized to 0 tokens. */
  buttonSemaphoreHandle = osSemaphoreNew(1, 0, &buttonSemaphore_attributes);
  if (buttonSemaphoreHandle == NULL) {
      UART_Log("CRITICAL ERROR: Failed to create button binary semaphore!");
  }
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* Create software timer for manual watering. Runs as a one-shot 10-minute timer. */
  manualWateringTimerHandle = osTimerNew(ManualWateringTimer_Callback, osTimerOnce, NULL, &manualWateringTimer_attributes);
  if (manualWateringTimerHandle == NULL) {
      UART_Log("CRITICAL ERROR: Failed to create manual watering timer!");
  }

  /* Create and start periodic software timer for LED blinking (100ms period) to save RAM */
  ledTimerHandle = osTimerNew(LEDTimer_Callback, osTimerPeriodic, NULL, &ledTimer_attributes);
  if (ledTimerHandle != NULL) {
      osTimerStart(ledTimerHandle, pdMS_TO_TICKS(100));
  } else {
      UART_Log("CRITICAL ERROR: Failed to create LED timer!");
  }
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* Create message queue for sensor values. Capacity is 2 items of Greenhouse_Sensors_t. */
  greenhouseQueueHandle = osMessageQueueNew(2, sizeof(Greenhouse_Sensors_t), &greenhouseQueue_attributes);
  if (greenhouseQueueHandle == NULL) {
      UART_Log("CRITICAL ERROR: Failed to create greenhouse message queue!");
  }
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* Create reading task */
  readTaskHandle = osThreadNew(Task_Read_Rtn, NULL, &readTask_attributes);
  if (readTaskHandle == NULL) {
      UART_Log("CRITICAL ERROR: Failed to create read task!");
  }
  
  /* Direct boot diagnostics to UART */
  HAL_UART_Transmit(&huart1, (uint8_t*)"\r\n--- BOOT CHECK ---\r\n", 22, HAL_MAX_DELAY);
  if (uartMutexHandle == NULL) HAL_UART_Transmit(&huart1, (uint8_t*)"[ERR] uartMutex is NULL!\r\n", 26, HAL_MAX_DELAY);
  else HAL_UART_Transmit(&huart1, (uint8_t*)"[OK] uartMutex is created\r\n", 27, HAL_MAX_DELAY);
  
  if (buttonSemaphoreHandle == NULL) HAL_UART_Transmit(&huart1, (uint8_t*)"[ERR] buttonSemaphore is NULL!\r\n", 32, HAL_MAX_DELAY);
  else HAL_UART_Transmit(&huart1, (uint8_t*)"[OK] buttonSemaphore is created\r\n", 33, HAL_MAX_DELAY);
  
  if (manualWateringTimerHandle == NULL) HAL_UART_Transmit(&huart1, (uint8_t*)"[ERR] manualWateringTimer is NULL!\r\n", 36, HAL_MAX_DELAY);
  else HAL_UART_Transmit(&huart1, (uint8_t*)"[OK] manualWateringTimer is created\r\n", 37, HAL_MAX_DELAY);
  
  if (ledTimerHandle == NULL) HAL_UART_Transmit(&huart1, (uint8_t*)"[ERR] ledTimer is NULL!\r\n", 25, HAL_MAX_DELAY);
  else HAL_UART_Transmit(&huart1, (uint8_t*)"[OK] ledTimer is created\r\n", 26, HAL_MAX_DELAY);
  
  if (greenhouseQueueHandle == NULL) HAL_UART_Transmit(&huart1, (uint8_t*)"[ERR] greenhouseQueue is NULL!\r\n", 32, HAL_MAX_DELAY);
  else HAL_UART_Transmit(&huart1, (uint8_t*)"[OK] greenhouseQueue is created\r\n", 33, HAL_MAX_DELAY);
  
  if (defaultTaskHandle == NULL) HAL_UART_Transmit(&huart1, (uint8_t*)"[ERR] defaultTask is NULL!\r\n", 28, HAL_MAX_DELAY);
  else HAL_UART_Transmit(&huart1, (uint8_t*)"[OK] defaultTask is created\r\n", 29, HAL_MAX_DELAY);
  
  if (readTaskHandle == NULL) HAL_UART_Transmit(&huart1, (uint8_t*)"[ERR] readTask is NULL!\r\n", 25, HAL_MAX_DELAY);
  else HAL_UART_Transmit(&huart1, (uint8_t*)"[OK] readTask is created\r\n", 26, HAL_MAX_DELAY);
  
  HAL_UART_Transmit(&huart1, (uint8_t*)"--- END BOOT CHECK ---\r\n\r\n", 26, HAL_MAX_DELAY);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  /* We reuse CubeMX's default task to run the main control logic.
     This saves us from allocating stack and TCB memory for a separate logic task. */
  Task_Logic_Rtn(argument);
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

