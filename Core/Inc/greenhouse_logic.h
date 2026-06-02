/**
  ******************************************************************************
  * @file    greenhouse_logic.h
  * @brief   Core business logic and RTOS task declarations for the greenhouse
  *          control system. Define message structs and task entry points.
  ******************************************************************************
  */

#ifndef GREENHOUSE_LOGIC_H
#define GREENHOUSE_LOGIC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cmsis_os.h"
#include "main.h"

/* --- Configuration Macros --- */

/* 
 * For simulation, we run the sensor reading task every 1 second (1000 ms) 
 * instead of 60 seconds (60000 ms). This allows observing 60/120 minute 
 * delays in 60/120 seconds in the terminal.
 * CHANGE to 60000 for production hardware deployment.
 */
#define GREENHOUSE_READ_INTERVAL_MS   1000

/* --- Mock Button and LED Pin Defines --- */
#ifndef BUTTON_Pin
#define BUTTON_Pin                    GPIO_PIN_0
#endif

#ifndef LD3_Pin
#define LD3_Pin                       GPIO_PIN_3
#define LD3_GPIO_Port                 GPIOB
#endif

/* --- Data Structures --- */

/**
 * @brief Structure containing sensor readings in fixed-point format (tenths of units).
 *        E.g., 245 = 24.5C, 552 = 55.2%
 */
typedef struct {
    int16_t temp1;       /**< Temperature from sensor 1 (Heater Logic) */
    int16_t temp2;       /**< Temperature from sensor 2 (Average calc) */
    int16_t temp3;       /**< Temperature from sensor 3 (Ventilation Flaps) */
    uint16_t humidity;   /**< Soil/air humidity percentage (AUTO Watering) */
} Greenhouse_Sensors_t;

/* --- RTOS Task Entry Points --- */

/**
 * @brief Task responsible for periodic polling of simulated/real sensors.
 *        Wakes up every GREENHOUSE_READ_INTERVAL_MS, reads data,
 *        and pushes it to the queue.
 */
void Task_Read_Rtn(void *argument);

/**
 * @brief Main logic processing task. Waits for data in the message queue,
 *        processes environmental rules (heater, flaps, auto watering),
 *        and commands the actuators.
 */
void Task_Logic_Rtn(void *argument);

/**
 * @brief Software timer callback function. Automatically triggered when the
 *        manual watering timer expires (after 10 minutes) to turn off watering.
 */
void ManualWateringTimer_Callback(void *argument);

/**
 * @brief Software timer callback function. Wakes up every 100ms to blink the
 *        green LED (LD3) based on the state of the actuators.
 */
void LEDTimer_Callback(void *argument);

/* --- Actuator State Getters --- */
uint8_t Greenhouse_Logic_GetHeaterState(void);
uint8_t Greenhouse_Logic_GetFlapsState(void);
uint8_t Greenhouse_Logic_GetWateringState(void);

/* --- External handles to CMSIS RTOS V2 objects (defined in freertos.c) --- */
extern osMessageQueueId_t greenhouseQueueHandle;
extern osTimerId_t manualWateringTimerHandle;
extern osTimerId_t ledTimerHandle;
extern osSemaphoreId_t buttonSemaphoreHandle;

#ifdef __cplusplus
}
#endif

#endif /* GREENHOUSE_LOGIC_H */
