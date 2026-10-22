/**
 ******************************************************************************
 * @file    greenhouse_logic.h
 * @brief   Core climate control logic, fixed-point sensor telemetry structures,
 *          and FreeRTOS task interface definitions.
 ******************************************************************************
 */

#ifndef GREENHOUSE_LOGIC_H
#define GREENHOUSE_LOGIC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cmsis_os.h"
#include "main.h"

/* ========================================================================== */
/* --- TIMING & SYSTEM CONFIGURATION ---------------------------------------- */
/* ========================================================================== */

/* Sensor polling interval for Task_Read.
 * Fast cycle (1000 ms) accelerates simulation during laboratory evaluation.
 * In field deployment, this should be scaled to 60000 ms to reduce sensor self-heating
 * and optimize energy consumption.
 */
#define GREENHOUSE_READ_INTERVAL_MS   1000

/* Duration of manual watering cycle triggered via external button interrupt (EXTI).
 * 10 seconds for bench validation; production deployment typically uses 10 minutes (600000 ms).
 */
#define MANUAL_WATERING_DURATION_MS   (10 * 1000)

/* Default GPIO fallback mappings for user button and status LED */
#ifndef BUTTON_Pin
#define BUTTON_Pin                    GPIO_PIN_0
#endif

#ifndef LD3_Pin
#define LD3_Pin                       GPIO_PIN_3
#define LD3_GPIO_Port                 GPIOB
#endif

/* ========================================================================== */
/* --- DATA STRUCTURES ------------------------------------------------------ */
/* ========================================================================== */

/* Fixed-point sensor telemetry container.
 * Scaled by 10 (e.g., 245 = 24.5 deg C, 552 = 55.2% RH).
 * Integer representation avoids floating-point software emulation on Cortex-M0,
 * saving ~2.4 KB of Flash and reducing ISR/task execution latency.
 */
typedef struct {
    int16_t temp1;       /* Sensor 1: Used for heating control */
    int16_t temp2;       /* Sensor 2: Intermediate canopy monitoring / average */
    int16_t temp3;       /* Sensor 3: Upper ventilation ridge monitoring */
    uint16_t humidity;   /* Soil/air relative humidity for irrigation logic */
} Greenhouse_Sensors_t;

/* ========================================================================== */
/* --- RTOS TASK & TIMER ENTRY POINTS --------------------------------------- */
/* ========================================================================== */

/* Periodic telemetry task: acquires sensor data and enqueues to logic task */
void Task_Read_Rtn(void *argument);

/* Main supervisory control task: evaluates hysteresis thresholds and drives actuators */
void Task_Logic_Rtn(void *argument);

/* One-shot timer callback: shuts down manual watering after timeout */
void ManualWateringTimer_Callback(void *argument);

/* Periodic timer callback (100 ms): modulates LD3 blink cadence to reflect actuator states */
void LEDTimer_Callback(void *argument);

/* ========================================================================== */
/* --- ACTUATOR STATE ACCESSORS --------------------------------------------- */
/* ========================================================================== */

uint8_t Greenhouse_Logic_GetHeaterState(void);
uint8_t Greenhouse_Logic_GetFlapsState(void);
uint8_t Greenhouse_Logic_GetWateringState(void);

/* ========================================================================== */
/* --- SYSTEM RTOS HANDLES -------------------------------------------------- */
/* ========================================================================== */

extern osMessageQueueId_t greenhouseQueueHandle;
extern osTimerId_t manualWateringTimerHandle;
extern osTimerId_t ledTimerHandle;
extern osSemaphoreId_t buttonSemaphoreHandle;

#ifdef __cplusplus
}
#endif

#endif /* GREENHOUSE_LOGIC_H */
