/**
 ******************************************************************************
 * @file    greenhouse_logic.c
 * @brief   Core greenhouse climate logic and FreeRTOS task implementations.
 *
 *          Design Note: Standard library printf/snprintf is deliberately avoided.
 *          On Cortex-M0 (ARMv6-M) with 4 KB RAM, newlib-nano consumes ~3 KB Flash
 *          and allocates heavy re-entrant structures on the task stack (>200 B),
 *          which triggers stack overflow and unrecoverable HardFaults.
 *          String telemetry is formatted via lightweight static integer conversion.
 ******************************************************************************
 */

#include "greenhouse_logic.h"
#include "FreeRTOS.h"
#include "greenhouse_hal_wrapper.h"
#include "task.h"
#include <string.h>

/* ========================================================================== */
/* --- DEFINITIONS & SYSTEM STATE ------------------------------------------- */
/* ========================================================================== */

typedef enum { 
    HEATER_STATE_OFF = 0, 
    HEATER_STATE_ON 
} HeaterState_t;

typedef enum { 
    FLAPS_STATE_CLOSED = 0, 
    FLAPS_STATE_OPEN 
} FlapsState_t;

/* Volatile flags accessed across supervisory task, timer callbacks, and ISR */
static volatile uint8_t auto_watering_active = 0;
static volatile uint8_t manual_watering_active = 0;

/* Cached actuator states for status LED cadence timer */
static volatile uint8_t current_heater_state = 0;
static volatile uint8_t current_flaps_state = 0;

/* ========================================================================== */
/* --- PRIVATE FUNCTION PROTOTYPES ------------------------------------------ */
/* ========================================================================== */

/* Evaluates combined AUTO and MANUAL watering requirements and switches solenoid */
static void Greenhouse_Logic_UpdateWateringState(void);

/* Generates ASCII telemetry frame and transmits over UART without dynamic allocations */
static void Log_GreenhouseState(const Greenhouse_Sensors_t *data);

/* ========================================================================== */
/* --- ACTUATOR STATE ACCESSORS --------------------------------------------- */
/* ========================================================================== */

uint8_t Greenhouse_Logic_GetHeaterState(void) { 
    return current_heater_state; 
}

uint8_t Greenhouse_Logic_GetFlapsState(void) { 
    return current_flaps_state; 
}

uint8_t Greenhouse_Logic_GetWateringState(void) {
    return (auto_watering_active || manual_watering_active);
}

/* ========================================================================== */
/* --- FREERTOS TASKS ------------------------------------------------------- */
/* ========================================================================== */

/**
 * @brief  Periodic telemetry acquisition task.
 *         Periodically queries HAL sensor endpoints and enqueues data packets
 *         for asynchronous consumption by Task_Logic_Rtn.
 */
void Task_Read_Rtn(void *argument) {
    (void)argument;
    UART_Log("DBG: readTask started");
    Greenhouse_Sensors_t sensor_data;

    for (;;) {
        /* Advance testbench profile (in production: replaces with I2C/ADC reads) */
        Greenhouse_HAL_SimulateStep();

        sensor_data.temp1 = GetTemp1();
        sensor_data.temp2 = GetTemp2();
        sensor_data.temp3 = GetTemp3();
        sensor_data.humidity = GetHumidity();

        /* Enqueue telemetry packet using a bounded timeout (100 ms).
         * Avoids infinite blocking if the consumer task encounters an exception.
         */
        osStatus_t status = osMessageQueuePut(greenhouseQueueHandle, &sensor_data,
                                              0, pdMS_TO_TICKS(100));
        if (status != osOK) {
            UART_Log("ERROR: Failed to push sensor data to queue!");
        }

        /* Relinquish CPU to lower-priority threads during sampling interval */
        osDelay(pdMS_TO_TICKS(GREENHOUSE_READ_INTERVAL_MS));
    }
}

/**
 * @brief  Supervisory greenhouse logic task.
 *         Consumes telemetry frames, executes multi-point hysteresis algorithms,
 *         and monitors manual override flags asynchronously.
 */
void Task_Logic_Rtn(void *argument) {
    (void)argument;
    UART_Log("DBG: defaultTask started");
    Greenhouse_Sensors_t data;

    HeaterState_t heater_state = HEATER_STATE_OFF;
    FlapsState_t flaps_state = FLAPS_STATE_CLOSED;

    uint32_t temp1_low_minutes = 0;
    uint32_t temp3_high_minutes = 0;

    for (;;) {
        /* Block on telemetry queue with 100 ms timeout to maintain responsiveness
         * for asynchronous operator input without busy-waiting.
         */
        osStatus_t status = osMessageQueueGet(greenhouseQueueHandle, &data, NULL,
                                              pdMS_TO_TICKS(100));

        if (status == osOK) {
            Log_GreenhouseState(&data);

            /* --- 1. HEATING CONTROL LOOP --- */
            /* Spatial mean calculation across all three canopy sensors */
            int16_t avg_temp = (int16_t)(((int32_t)data.temp1 + data.temp2 + data.temp3) / 3);

            if (heater_state == HEATER_STATE_OFF) {
                /* Emergency freeze protection: trigger immediate heating without dwell time
                 * 10 = 1.0 deg C, 50 = 5.0 deg C
                 */
                if (data.temp1 <= 10 || avg_temp <= 50) {
                    heater_state = HEATER_STATE_ON;
                    current_heater_state = 1;
                    HeaterOn();
                    UART_Log("ALARM: Critical temp! Heater ON!");
                }
                /* Standard heating demand: Temp1 < 4.0 deg C sustained for 5 consecutive sample cycles */
                else if (data.temp1 < 40) {
                    temp1_low_minutes++;
                    if (temp1_low_minutes >= 5) {
                        heater_state = HEATER_STATE_ON;
                        current_heater_state = 1;
                        HeaterOn();
                        UART_Log("SYSTEM: Temp1 low 5m. Heater ON.");
                    }
                }
                /* Reset persistence timer on transient temperature rise */
                else {
                    temp1_low_minutes = 0;
                }
            } else {
                /* Heating cut-off hysteresis: turn off when temperature recovers to >= 6.0 deg C.
                 * Prevents contactor chatter around switching boundary.
                 */
                if (data.temp1 >= 60) {
                    heater_state = HEATER_STATE_OFF;
                    current_heater_state = 0;
                    HeaterOff();
                    temp1_low_minutes = 0;
                    UART_Log("SYSTEM: Target reached. Heater OFF.");
                }
            }

            /* --- 2. VENTILATION RIDGE FLAPS LOOP --- */
            if (flaps_state == FLAPS_STATE_CLOSED) {
                /* Open ridge flaps if upper thermal layer (Temp3) exceeds 28.0 deg C for 10 cycles */
                if (data.temp3 > 280) {
                    temp3_high_minutes++;
                    if (temp3_high_minutes >= 10) {
                        flaps_state = FLAPS_STATE_OPEN;
                        current_flaps_state = 1;
                        AirFlapsOpen();
                        UART_Log("SYSTEM: Temp3 high 10m. Flaps OPEN.");
                    }
                } else {
                    temp3_high_minutes = 0;
                }
            } else {
                /* Immediate closure when temperature drops to <= 24.0 deg C to conserve heat */
                if (data.temp3 <= 240) {
                    flaps_state = FLAPS_STATE_CLOSED;
                    current_flaps_state = 0;
                    AirFlapsClose();
                    temp3_high_minutes = 0;
                    UART_Log("SYSTEM: Temp3 cooled. Flaps CLOSE.");
                }
            }

            /* --- 3. AUTOMATED IRRIGATION LOOP --- */
            if (!auto_watering_active) {
                /* Activate sprinklers when relative humidity falls below 40.0% */
                if (data.humidity < 400) {
                    auto_watering_active = 1;
                    UART_Log("SYSTEM: Humidity <40%. Sprinklers ON.");
                    Greenhouse_Logic_UpdateWateringState();
                }
            } else {
                /* De-energize sprinklers once soil reaches 60.0% saturation threshold */
                if (data.humidity >= 600) {
                    auto_watering_active = 0;
                    UART_Log("SYSTEM: Humidity >=60%. Sprinklers OFF.");
                    Greenhouse_Logic_UpdateWateringState();
                }
            }
        }

        /* --- 4. MANUAL OVERRIDE PROCESSING --- */
        /* Non-blocking semaphore acquisition from hardware button interrupt (EXTI) */
        if (buttonSemaphoreHandle != NULL) {
            if (osSemaphoreAcquire(buttonSemaphoreHandle, 0) == osOK) {
                UART_Log("USER: Manual button interrupt detected.");

                manual_watering_active = 1;
                Greenhouse_Logic_UpdateWateringState();

                /* Arm or re-trigger single-shot timeout timer */
                if (manualWateringTimerHandle != NULL) {
                    osStatus_t timer_status =
                        osTimerStart(manualWateringTimerHandle,
                                     pdMS_TO_TICKS(MANUAL_WATERING_DURATION_MS));
                    if (timer_status == osOK) {
                        UART_Log("SYSTEM: Manual timer started.");
                    } else {
                        UART_Log("ERROR: Failed to start manual timer!");
                    }
                }
            }
        }
    }
}

/* ========================================================================== */
/* --- SYSTEM TIMER CALLBACKS ----------------------------------------------- */
/* ========================================================================== */

/**
 * @brief  One-shot timer callback: clears manual irrigation upon timeout expiration.
 */
void ManualWateringTimer_Callback(void *argument) {
    (void)argument;
    UART_Log("SYSTEM: Manual timer expired.");

    manual_watering_active = 0;
    Greenhouse_Logic_UpdateWateringState();
}

/**
 * @brief  Periodic timer callback (100 ms): visual telemetry via onboard LED (LD3).
 *         Priority hierarchy:
 *         1. Solid ON: Irrigation active (auto or manual)
 *         2. Fast blink (5 Hz): Heating active
 *         3. Slow blink (1 Hz): Ventilation open
 *         4. Heartbeat pulse (0.5 Hz): System idle / healthy
 */
void LEDTimer_Callback(void *argument) {
    (void)argument;
    static uint32_t ticks = 0;
    ticks++;

    uint8_t heater = Greenhouse_Logic_GetHeaterState();
    uint8_t watering = Greenhouse_Logic_GetWateringState();
    uint8_t flaps = Greenhouse_Logic_GetFlapsState();

    if (watering) {
        /* Solid illumination: highest operational priority */
        HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_SET);
    } else if (heater) {
        /* Fast toggle (5 Hz, toggles every 100 ms) indicates active thermal demand */
        HAL_GPIO_TogglePin(LD3_GPIO_Port, LD3_Pin);
    } else if (flaps) {
        /* Slow toggle (1 Hz, toggles every 500 ms / 5 ticks) indicates open canopy vents */
        if (ticks % 5 == 0) {
            HAL_GPIO_TogglePin(LD3_GPIO_Port, LD3_Pin);
        }
    } else {
        /* Heartbeat pattern: short pulse every 2 seconds (20 ticks) */
        if (ticks % 20 == 0) {
            HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_SET);
        } else if (ticks % 20 == 1) {
            HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);
        }
    }
}

/* ========================================================================== */
/* --- HARDWARE INTERRUPT (EXTI) HANDLERS ----------------------------------- */
/* ========================================================================== */

/**
 * @brief  External GPIO interrupt callback (Falling edge on PA0).
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    if (GPIO_Pin == BUTTON_Pin) {
        /* Release binary semaphore to unblock logic thread.
         * CMSIS-RTOS V2 safely routes this call to xSemaphoreGiveFromISR in ISR context.
         */
        if (buttonSemaphoreHandle != NULL) {
            osSemaphoreRelease(buttonSemaphoreHandle);
        }
    }
}

/* ========================================================================== */
/* --- HELPER & STRING FORMATTING ROUTINES ---------------------------------- */
/* ========================================================================== */

/**
 * @brief  Appends fixed-point signed integer (1 decimal place) to ASCII buffer.
 *         Protected against signed 16-bit underflow (INT16_MIN).
 */
static uint8_t append_fixed(char *buf, uint8_t pos, int16_t val) {
    int32_t v = val;
    if (v < 0) {
        buf[pos++] = '-';
        v = -v;
    }
    int32_t whole = v / 10;

    /* Up to 3 digits before decimal point (covers range up to 999.9) */
    if (whole >= 100) {
        buf[pos++] = (char)('0' + (whole / 100) % 10);
    }
    if (whole >= 10) {
        buf[pos++] = (char)('0' + (whole / 10) % 10);
    }
    buf[pos++] = (char)('0' + whole % 10);
    buf[pos++] = '.';
    buf[pos++] = (char)('0' + v % 10);
    return pos;
}

/**
 * @brief  Thread-safe, critical-section guarded actuator state update.
 */
static void Greenhouse_Logic_UpdateWateringState(void) {
    static uint8_t last_watering_state = 0;
    uint8_t should_be_on = 0;
    uint8_t state_changed = 0;

    /* Protect composite flags against concurrent access between ISR and timer callbacks */
    taskENTER_CRITICAL();
    should_be_on = (auto_watering_active || manual_watering_active);
    if (should_be_on != last_watering_state) {
        last_watering_state = should_be_on;
        state_changed = 1;
    }
    taskEXIT_CRITICAL();

    /* Execute physical IO and serial telemetry outside critical section */
    if (state_changed) {
        if (should_be_on) {
            WateringOn();
        } else {
            WateringOff();
        }
    }
}

/**
 * @brief  Formats and logs periodic telemetry line: "T:t1,t2,t3 H:humidity".
 */
static void Log_GreenhouseState(const Greenhouse_Sensors_t *data) {
    /* Static allocation avoids allocating stack buffers in memory-constrained thread frame */
    static char log_buffer[36];
    uint8_t pos = 0;

    log_buffer[pos++] = 'T';
    log_buffer[pos++] = ':';
    pos = append_fixed(log_buffer, pos, data->temp1);
    log_buffer[pos++] = ',';
    pos = append_fixed(log_buffer, pos, data->temp2);
    log_buffer[pos++] = ',';
    pos = append_fixed(log_buffer, pos, data->temp3);
    log_buffer[pos++] = ' ';
    log_buffer[pos++] = 'H';
    log_buffer[pos++] = ':';
    pos = append_fixed(log_buffer, pos, (int16_t)data->humidity);
    log_buffer[pos] = '\0';

    UART_Log_Lock();
    UART_Log_Direct(log_buffer);
    UART_Log_Unlock();
}

