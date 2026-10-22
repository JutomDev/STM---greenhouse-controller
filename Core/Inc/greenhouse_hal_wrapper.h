/**
 ******************************************************************************
 * @file    greenhouse_hal_wrapper.h
 * @brief   Hardware Abstraction Layer (HAL) interface definitions for sensors,
 *          actuators, UART telemetry logging, and testbench environmental simulation.
 ******************************************************************************
 */

#ifndef GREENHOUSE_HAL_WRAPPER_H
#define GREENHOUSE_HAL_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "cmsis_os.h"

/* ========================================================================== */
/* --- SYSTEM RTOS HANDLES -------------------------------------------------- */
/* ========================================================================== */

/* Mutex protecting non-atomic USART1 transmissions across concurrent threads */
extern osMutexId_t uartMutexHandle;

/* ========================================================================== */
/* --- SENSOR TELEMETRY INTERFACES ------------------------------------------ */
/* ========================================================================== */

/* Read temperature sensor 1 (Heating control loop).
 * Returns fixed-point deci-degrees Celsius (e.g., 245 = 24.5 deg C).
 */
int16_t GetTemp1(void);

/* Read temperature sensor 2 (Canopy spatial average loop).
 * Returns fixed-point deci-degrees Celsius.
 */
int16_t GetTemp2(void);

/* Read temperature sensor 3 (Ventilation ridge control loop).
 * Returns fixed-point deci-degrees Celsius.
 */
int16_t GetTemp3(void);

/* Read soil/ambient relative humidity sensor (Irrigation control loop).
 * Returns fixed-point deci-percent (e.g., 552 = 55.2% RH).
 */
uint16_t GetHumidity(void);

/* ========================================================================== */
/* --- ACTUATOR CONTROL INTERFACES ------------------------------------------ */
/* ========================================================================== */

/* Energize heating element contactor */
void HeaterOn(void);

/* De-energize heating element contactor */
void HeaterOff(void);

/* Open greenhouse ventilation ridge flaps */
void AirFlapsOpen(void);

/* Close greenhouse ventilation ridge flaps */
void AirFlapsClose(void);

/* Energize irrigation solenoid valve */
void WateringOn(void);

/* De-energize irrigation solenoid valve */
void WateringOff(void);

/* ========================================================================== */
/* --- USER INTERACTION INTERFACES ------------------------------------------ */
/* ========================================================================== */

/* Non-blocking query of manual watering button state (1 = pressed, 0 = released) */
uint8_t IsWateringButtonPressed(void);

/* ========================================================================== */
/* --- SERIAL LOGGING INTERFACES (USART1) ----------------------------------- */
/* ========================================================================== */

/* Transmit log string with automatic thread-safe mutex acquisition */
void UART_Log(const char* msg);

/* Acquire UART mutex for multi-part formatted message assembly */
void UART_Log_Lock(void);

/* Release UART mutex */
void UART_Log_Unlock(void);

/* Low-level UART line transmit. Caller must hold uartMutexHandle */
void UART_Log_Direct(const char* msg);

/* ========================================================================== */
/* --- HARDWARE-IN-THE-LOOP / TESTBENCH SIMULATOR --------------------------- */
/* ========================================================================== */

/* Advance internal environmental simulation state machine.
 * Emulates dynamic greenhouse temperature and moisture curves for validation
 * when physical sensors are not attached.
 */
void Greenhouse_HAL_SimulateStep(void);

#ifdef __cplusplus
}
#endif

#endif /* GREENHOUSE_HAL_WRAPPER_H */
