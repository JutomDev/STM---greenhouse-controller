/**
  ******************************************************************************
  * @file    greenhouse_hal_wrapper.h
  * @brief   Hardware Abstraction Layer (HAL) wrapper declarations for the
  *          greenhouse control system. Provides mock interfaces for sensors,
  *          actuators, and system logging.
  ******************************************************************************
  */

#ifndef GREENHOUSE_HAL_WRAPPER_H
#define GREENHOUSE_HAL_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "cmsis_os.h"

/* --- External Mutex Handle --- */
extern osMutexId_t uartMutexHandle;

/* --- Sensor Readings --- */

/**
 * @brief Get temperature from sensor 1 (used for heater control).
 * @return Temperature in tenths of degrees Celsius (e.g. 245 = 24.5C).
 */
int16_t GetTemp1(void);

/**
 * @brief Get temperature from sensor 2 (used for average temperature calculations).
 * @return Temperature in tenths of degrees Celsius.
 */
int16_t GetTemp2(void);

/**
 * @brief Get temperature from sensor 3 (used for air flaps control).
 * @return Temperature in tenths of degrees Celsius.
 */
int16_t GetTemp3(void);

/**
 * @brief Get relative humidity from soil/air sensor.
 * @return Relative humidity in tenths of percentage (e.g. 552 = 55.2%).
 */
uint16_t GetHumidity(void);


/* --- Actuator Controls --- */

/**
 * @brief Turn the heating system ON.
 */
void HeaterOn(void);

/**
 * @brief Turn the heating system OFF.
 */
void HeaterOff(void);

/**
 * @brief Open the greenhouse air ventilation flaps.
 */
void AirFlapsOpen(void);

/**
 * @brief Close the greenhouse air ventilation flaps.
 */
void AirFlapsClose(void);

/**
 * @brief Turn the watering system (sprinklers) ON.
 */
void WateringOn(void);

/**
 * @brief Turn the watering system (sprinklers) OFF.
 */
void WateringOff(void);


/* --- User Inputs --- */

/**
 * @brief Read state of the manual watering button.
 * @return 1 if pressed, 0 otherwise.
 */
uint8_t IsWateringButtonPressed(void);


/* --- Communication / Logging --- */

/**
 * @brief Log a system status or warning message via UART.
 * @param msg Null-terminated string containing the log message.
 */
void UART_Log(const char* msg);

/**
 * @brief Lock the UART mutex to protect shared static buffers and serial lines.
 */
void UART_Log_Lock(void);

/**
 * @brief Unlock the UART mutex after logging is completed.
 */
void UART_Log_Unlock(void);

/**
 * @brief Directly write to UART bypass-locking (caller must already hold the mutex).
 * @param msg Null-terminated string containing the log message.
 */
void UART_Log_Direct(const char* msg);


/* --- Simulation Control --- */

/**
 * @brief Perform a single step of the simulation. This updates the simulated sensor
 *        values and inputs to verify the system logic dynamically in the terminal.
 *        Called automatically in the sensor reading task.
 */
void Greenhouse_HAL_SimulateStep(void);

#ifdef __cplusplus
}
#endif

#endif /* GREENHOUSE_HAL_WRAPPER_H */
