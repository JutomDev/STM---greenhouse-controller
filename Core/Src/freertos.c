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
/* Static memory allocation for FreeRTOS Task Control Blocks (TCB), queues, and stacks.
 * Eliminates heap fragmentation and avoids runtime out-of-memory faults on 4 KB SRAM.
 */

/* readTask allocation: 48 words (192 bytes) stack is strictly sufficient since it performs
 * fixed-point sensor reads and non-allocating queue insertion without recursive calls.
 */
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

/* greenhouseQueue allocation: depth = 2 frames.
 * Depth of 2 handles burst telemetry between sensor polling and logic processing
 * while minimizing static SRAM consumption to 16 bytes payload + control block.
 */
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

/* manualWateringTimer allocation: single-shot software timer */
osTimerId_t manualWateringTimerHandle;
static StaticTimer_t manualWateringTimerControlBlock;
const osTimerAttr_t manualWateringTimer_attributes = {
  .name = "manualWateringTimer",
  .cb_mem = &manualWateringTimerControlBlock,
  .cb_size = sizeof(manualWateringTimerControlBlock),
};

/* ledTimer allocation: periodic software timer (100 ms) for visual telemetry */
osTimerId_t ledTimerHandle;
static StaticTimer_t ledTimerControlBlock;
const osTimerAttr_t ledTimer_attributes = {
  .name = "ledTimer",
  .cb_mem = &ledTimerControlBlock,
  .cb_size = sizeof(ledTimerControlBlock),
};

/* uartMutex allocation: binary mutex preventing serial log interleaving */
osMutexId_t uartMutexHandle;
static StaticSemaphore_t uartMutexControlBlock;
const osMutexAttr_t uartMutex_attributes = {
  .name = "uartMutex",
  .cb_mem = &uartMutexControlBlock,
  .cb_size = sizeof(uartMutexControlBlock),
};

/* buttonSemaphore allocation: binary semaphore signaling EXTI0 edge trigger to logic task */
osSemaphoreId_t buttonSemaphoreHandle;
static StaticSemaphore_t buttonSemaphoreControlBlock;
const osSemaphoreAttr_t buttonSemaphore_attributes = {
  .name = "buttonSemaphore",
  .cb_mem = &buttonSemaphoreControlBlock,
  .cb_size = sizeof(buttonSemaphoreControlBlock),
};

/* defaultTask (logicTask) allocation: 100 words (400 bytes) stack */
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
  uartMutexHandle = osMutexNew(&uartMutex_attributes);
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* Binary semaphore initialized to 0 tokens (waits for EXTI ISR give) */
  buttonSemaphoreHandle = osSemaphoreNew(1, 0, &buttonSemaphore_attributes);
  if (buttonSemaphoreHandle == NULL) {
      UART_Log("CRITICAL ERROR: Failed to create button binary semaphore!");
  }
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  manualWateringTimerHandle = osTimerNew(ManualWateringTimer_Callback, osTimerOnce, NULL, &manualWateringTimer_attributes);
  if (manualWateringTimerHandle == NULL) {
      UART_Log("CRITICAL ERROR: Failed to create manual watering timer!");
  }

  /* Periodic cadence timer for onboard visual signaling */
  ledTimerHandle = osTimerNew(LEDTimer_Callback, osTimerPeriodic, NULL, &ledTimer_attributes);
  if (ledTimerHandle != NULL) {
      osTimerStart(ledTimerHandle, pdMS_TO_TICKS(100));
  } else {
      UART_Log("CRITICAL ERROR: Failed to create LED timer!");
  }
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  greenhouseQueueHandle = osMessageQueueNew(2, sizeof(Greenhouse_Sensors_t), &greenhouseQueue_attributes);
  if (greenhouseQueueHandle == NULL) {
      UART_Log("CRITICAL ERROR: Failed to create greenhouse message queue!");
  }
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  readTaskHandle = osThreadNew(Task_Read_Rtn, NULL, &readTask_attributes);
  if (readTaskHandle == NULL) {
      UART_Log("CRITICAL ERROR: Failed to create read task!");
  }
  
  /* Initial boot diagnostics emitted over raw UART before scheduler launch */
  HAL_UART_Transmit(&huart1, (uint8_t*)"\r\n--- BOOT CHECK ---\r\n", 22, 100);
  if (uartMutexHandle == NULL) HAL_UART_Transmit(&huart1, (uint8_t*)"[ERR] uartMutex is NULL!\r\n", 26, 100);
  else HAL_UART_Transmit(&huart1, (uint8_t*)"[OK] uartMutex is created\r\n", 27, 100);
  
  if (buttonSemaphoreHandle == NULL) HAL_UART_Transmit(&huart1, (uint8_t*)"[ERR] buttonSemaphore is NULL!\r\n", 32, 100);
  else HAL_UART_Transmit(&huart1, (uint8_t*)"[OK] buttonSemaphore is created\r\n", 33, 100);
  
  if (manualWateringTimerHandle == NULL) HAL_UART_Transmit(&huart1, (uint8_t*)"[ERR] manualWateringTimer is NULL!\r\n", 36, 100);
  else HAL_UART_Transmit(&huart1, (uint8_t*)"[OK] manualWateringTimer is created\r\n", 37, 100);
  
  if (ledTimerHandle == NULL) HAL_UART_Transmit(&huart1, (uint8_t*)"[ERR] ledTimer is NULL!\r\n", 25, 100);
  else HAL_UART_Transmit(&huart1, (uint8_t*)"[OK] ledTimer is created\r\n", 26, 100);
  
  if (greenhouseQueueHandle == NULL) HAL_UART_Transmit(&huart1, (uint8_t*)"[ERR] greenhouseQueue is NULL!\r\n", 32, 100);
  else HAL_UART_Transmit(&huart1, (uint8_t*)"[OK] greenhouseQueue is created\r\n", 33, 100);
  
  if (defaultTaskHandle == NULL) HAL_UART_Transmit(&huart1, (uint8_t*)"[ERR] defaultTask is NULL!\r\n", 28, 100);
  else HAL_UART_Transmit(&huart1, (uint8_t*)"[OK] defaultTask is created\r\n", 29, 100);
  
  if (readTaskHandle == NULL) HAL_UART_Transmit(&huart1, (uint8_t*)"[ERR] readTask is NULL!\r\n", 25, 100);
  else HAL_UART_Transmit(&huart1, (uint8_t*)"[OK] readTask is created\r\n", 26, 100);
  
  HAL_UART_Transmit(&huart1, (uint8_t*)"--- END BOOT CHECK ---\r\n\r\n", 26, 100);
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
  /* Re-use defaultTask entry to host the supervisory greenhouse logic loop,
   * avoiding the RAM overhead of spawning a third thread.
   */
  Task_Logic_Rtn(argument);
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

