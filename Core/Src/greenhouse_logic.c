/**
  ******************************************************************************
  * @file    greenhouse_logic.c
  * @brief   Core business logic and RTOS task implementations for the greenhouse
  *          control system. Handles state machines using fixed-point math,
  *          hysterese, EXTI interrupt sync, and UART logging.
  *
  *          IMPORTANT: snprintf is intentionally NOT used. On Cortex-M0 with
  *          newlib-nano it costs ~3KB FLASH and 200-300B stack per call.
  *          All output uses manual char-by-char construction below.
  ******************************************************************************
  */

#include "greenhouse_logic.h"
#include "greenhouse_hal_wrapper.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* --- Private Defines --- */

/* 
 * Manual watering duration. 
 * Default: 10 minutes (600,000 ms). 
 * Change to e.g. 5000 (5 seconds) for rapid testing of the timer expiration.
 */
#define MANUAL_WATERING_DURATION_MS   (10 * 1000)

/* --- Private Typedefs --- */

typedef enum {
    HEATER_STATE_OFF = 0,
    HEATER_STATE_ON
} HeaterState_t;

typedef enum {
    FLAPS_STATE_CLOSED = 0,
    FLAPS_STATE_OPEN
} FlapsState_t;

/* --- Private Variables --- */

/* Global flags representing the status of automatic and manual watering.
   Marked volatile as they are modified across tasks and timer callbacks. */
static volatile uint8_t auto_watering_active = 0;
static volatile uint8_t manual_watering_active = 0;

/* Actuator state flags accessed by the LED timer callback */
static volatile uint8_t current_heater_state = 0;
static volatile uint8_t current_flaps_state = 0;

/* --- Private Function Prototypes --- */

/**
 * @brief Thread-safe update of the physical watering output based on 
 *        combining the active AUTO and MANUAL states.
 */
static void Greenhouse_Logic_UpdateWateringState(void);

/**
 * @brief Log current sensor values using safe snprintf and fixed-point math
 *        (avoiding float/double string formatting to prevent stack bloat).
 */
static void Log_GreenhouseState(const Greenhouse_Sensors_t* data);

/* --- Actuator State Getters --- */

uint8_t Greenhouse_Logic_GetHeaterState(void) {
    return current_heater_state;
}

uint8_t Greenhouse_Logic_GetFlapsState(void) {
    return current_flaps_state;
}

uint8_t Greenhouse_Logic_GetWateringState(void) {
    return (auto_watering_active || manual_watering_active);
}

/* --- RTOS Task Implementations --- */

/**
 * @brief Task responsible for periodic polling of simulated/real sensors.
 *        Wakes up every GREENHOUSE_READ_INTERVAL_MS, reads data,
 *        and pushes it to the queue.
 */
void Task_Read_Rtn(void *argument) {
    (void)argument;
    UART_Log("DBG: readTask started");
    Greenhouse_Sensors_t sensor_data;

    for (;;) {
        /* Advance the simulation values. In a production environment, this function
           would be replaced by actual ADC conversions / sensor read operations. */
        Greenhouse_HAL_SimulateStep();

        /* Collect data from physical or mocked interfaces (stored in fixed-point) */
        sensor_data.temp1 = GetTemp1();
        sensor_data.temp2 = GetTemp2();
        sensor_data.temp3 = GetTemp3();
        sensor_data.humidity = GetHumidity();

        /* Send collected readings to the logic processing queue.
           We use osWaitForever to block if the queue is full. */
        osStatus_t status = osMessageQueuePut(greenhouseQueueHandle, &sensor_data, 0, osWaitForever);
        if (status != osOK) {
            UART_Log("ERROR: Failed to push sensor data to queue!");
        }

        /* Wait for the next sampling period. Using RTOS ticks prevents CPU hogging. */
        osDelay(pdMS_TO_TICKS(GREENHOUSE_READ_INTERVAL_MS));
    }
}

/**
 * @brief Main logic processing task. Waits for data in the message queue,
 *        processes environmental rules (heater, flaps, auto watering),
 *        and commands the actuators.
 */
void Task_Logic_Rtn(void *argument) {
    (void)argument;
    UART_Log("DBG: defaultTask started");
    Greenhouse_Sensors_t data;
    
    /* State variables to keep track of the controller's state machines */
    HeaterState_t heater_state = HEATER_STATE_OFF;
    FlapsState_t flaps_state = FLAPS_STATE_CLOSED;
    
    uint32_t temp1_low_minutes = 0;
    uint32_t temp3_high_minutes = 0;

    for (;;) {
        /* Block on the queue with a 100ms timeout rather than osWaitForever.
           This allows us to periodically release block to check and handle the manual
           watering button semaphore, avoiding the need for an extra task (saving RAM). */
        osStatus_t status = osMessageQueueGet(greenhouseQueueHandle, &data, NULL, pdMS_TO_TICKS(100));
        
        if (status == osOK) {
            /* Log the received values safely using integer formatting */
            Log_GreenhouseState(&data);

            /* =================================================================
             * 1. HEATING SYSTEM LOGIC (Heater)
             * ================================================================= */
            /* Average temperature using fixed-point math */
            int16_t avg_temp = (data.temp1 + data.temp2 + data.temp3) / 3;

            if (heater_state == HEATER_STATE_OFF) {
                /* CRITICAL EXCEPTION: Turn on immediately if temperature drops to dangerous levels.
                   10 = 1.0C, 50 = 5.0C */
                if (data.temp1 <= 10 || avg_temp <= 50) {
                    heater_state = HEATER_STATE_ON;
                    current_heater_state = 1;
                    HeaterOn();
                    UART_Log("ALARM: Critical temp! Heater ON!");
                }
                /* STANDARD RULE: Turn on if Temp1 < 4.0C (40) for 60 consecutive reading intervals. */
                else if (data.temp1 < 40) {
                    temp1_low_minutes++;
                    if (temp1_low_minutes >= 5) {
                        heater_state = HEATER_STATE_ON;
                        current_heater_state = 1;
                        HeaterOn();
                        UART_Log("SYSTEM: Temp1 low 5m. Heater ON.");
                    }
                }
                /* Reset counter if temperature goes back above the trigger threshold
                   while the heater is still OFF. */
                else {
                    temp1_low_minutes = 0;
                }
            } 
            else { /* heater_state == HEATER_STATE_ON */
                /* HYSTERESIS RULE: Keep heater on until Temp1 reaches 6.0C (60) to avoid rapid
                   power cycling of the heating element (wear and efficiency protection). */
                if (data.temp1 >= 60) {
                    heater_state = HEATER_STATE_OFF;
                    current_heater_state = 0;
                    HeaterOff();
                    temp1_low_minutes = 0; /* Reset timer counter on power-down as requested */
                    UART_Log("SYSTEM: Target reached. Heater OFF.");
                }
            }

            /* =================================================================
             * 2. VENTILATION FLAPS LOGIC (Flaps)
             * ================================================================= */
            if (flaps_state == FLAPS_STATE_CLOSED) {
                /* STANDARD RULE: Open flaps if Temp3 > 28.0C (280) for 120 consecutive intervals. */
                if (data.temp3 > 280) {
                    temp3_high_minutes++;
                    if (temp3_high_minutes >= 10) {
                        flaps_state = FLAPS_STATE_OPEN;
                        current_flaps_state = 1;
                        AirFlapsOpen();
                        UART_Log("SYSTEM: Temp3 high 10m. Flaps OPEN.");
                    }
                }
                /* Reset counter if temperature drops below the opening threshold */
                else {
                    temp3_high_minutes = 0;
                }
            } 
            else { /* flaps_state == FLAPS_STATE_OPEN */
                /* HYSTERESIS RULE: Close flaps immediately if temperature drops to <= 24.0C (240)
                   to retain heat and avoid over-cooling the greenhouse. */
                if (data.temp3 <= 240) {
                    flaps_state = FLAPS_STATE_CLOSED;
                    current_flaps_state = 0;
                    AirFlapsClose();
                    temp3_high_minutes = 0;
                    UART_Log("SYSTEM: Temp3 cooled. Flaps CLOSE.");
                }
            }

            /* =================================================================
             * 3. AUTOMATIC WATERING LOGIC (Sprinklers AUTO)
             * ================================================================= */
            if (!auto_watering_active) {
                /* Turn on sprinklers if soil humidity falls below 40.0% (400) */
                if (data.humidity < 400) {
                    auto_watering_active = 1;
                    UART_Log("SYSTEM: Humidity <40%. Sprinklers ON.");
                    Greenhouse_Logic_UpdateWateringState();
                }
            } 
            else {
                /* Turn off sprinklers once target humidity of 60.0% (600) is reached */
                if (data.humidity >= 600) {
                    auto_watering_active = 0;
                    UART_Log("SYSTEM: Humidity >=60%. Sprinklers OFF.");
                    Greenhouse_Logic_UpdateWateringState();
                }
            }
        }

        /* =================================================================
         * 4. MANUAL WATERING SYNC (Semaphore from EXTI Interrupt)
         * ================================================================= */
        /* Check the binary semaphore released by EXTI ISR with 0 timeout (non-blocking). */
        if (buttonSemaphoreHandle != NULL) {
            if (osSemaphoreAcquire(buttonSemaphoreHandle, 0) == osOK) {
                UART_Log("USER: Manual button interrupt detected.");
                
                /* Activate manual watering override */
                manual_watering_active = 1;
                Greenhouse_Logic_UpdateWateringState();

                /* Start/restart the 10-minute software timer. */
                if (manualWateringTimerHandle != NULL) {
                    osStatus_t timer_status = osTimerStart(manualWateringTimerHandle, pdMS_TO_TICKS(MANUAL_WATERING_DURATION_MS));
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

/**
 * @brief Software timer callback function. Automatically triggered when the
 *        manual watering timer expires (after 10 minutes) to turn off watering.
 */
void ManualWateringTimer_Callback(void *argument) {
    (void)argument;
    UART_Log("SYSTEM: Manual timer expired.");
    
    /* Reset manual watering flag and re-evaluate physical sprinkler state */
    manual_watering_active = 0;
    Greenhouse_Logic_UpdateWateringState();
}

/**
 * @brief Software timer callback function. Wakes up every 100ms to blink the
 *        green LED (LD3) based on the state of the actuators.
 */
void LEDTimer_Callback(void *argument) {
    (void)argument;
    static uint32_t ticks = 0;
    ticks++;

    uint8_t heater = Greenhouse_Logic_GetHeaterState();
    uint8_t watering = Greenhouse_Logic_GetWateringState();
    uint8_t flaps = Greenhouse_Logic_GetFlapsState();

    if (watering) {
        /* Solid ON when watering is active */
        HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_SET);
    }
    else if (heater) {
        /* Fast blinking (100ms toggle -> 5Hz) when heater is active */
        HAL_GPIO_TogglePin(LD3_GPIO_Port, LD3_Pin);
    }
    else if (flaps) {
        /* Slow blinking (500ms toggle -> 1Hz) when flaps are open */
        if (ticks % 5 == 0) {
            HAL_GPIO_TogglePin(LD3_GPIO_Port, LD3_Pin);
        }
    }
    else {
        /* Idle pulse: flash for 100ms once every 2 seconds */
        if (ticks % 20 == 0) {
            HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_SET);
        } else if (ticks % 20 == 1) {
            HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);
        }
    }
}

/* --- Hardware Interrupt Callbacks (EXTI) --- */

/**
 * @brief  EXTI line detection callback. Replaces the button polling task
 *         with direct hardware interrupt-to-semaphore synchronization.
 * @param  GPIO_Pin: Specifies the pin connected to the EXTI line
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    if (GPIO_Pin == BUTTON_Pin) {
        /* Release the binary semaphore to wake up the control logic task.
           In CMSIS-RTOS V2, osSemaphoreRelease automatically handles calls from ISR. */
        if (buttonSemaphoreHandle != NULL) {
            osSemaphoreRelease(buttonSemaphoreHandle);
        }
    }
}

/* --- Private Helper Functions --- */

/**
 * @brief Thread-safe update of the physical watering output based on 
 *        combining the active AUTO and MANUAL states.
 */
static void Greenhouse_Logic_UpdateWateringState(void) {
    static uint8_t last_watering_state = 0;
    uint8_t should_be_on = 0;
    uint8_t state_changed = 0;

    /* Enter FreeRTOS critical section to prevent other tasks or the software timer
       callback from preempting and modifying variables during this check. */
    taskENTER_CRITICAL();
    should_be_on = (auto_watering_active || manual_watering_active);
    if (should_be_on != last_watering_state) {
        last_watering_state = should_be_on;
        state_changed = 1;
    }
    taskEXIT_CRITICAL();

    /* Perform actual hardware and logging updates outside of the critical section
       to keep interrupts disabled for as short a time as possible. */
    if (state_changed) {
        if (should_be_on) {
            WateringOn();
        } else {
            WateringOff();
        }
    }
}

/* --- Private Helper (local to this file) --- */

/**
 * @brief Append a signed fixed-point value (tenths) as "X.Y" into buf at pos.
 *        Returns updated position. Avoids snprintf to save FLASH and stack.
 */
static uint8_t append_fixed(char *buf, uint8_t pos, int16_t val)
{
    if (val < 0) {
        buf[pos++] = '-';
        val = (int16_t)(-val);
    }
    /* Tens and above digit(s) */
    int16_t whole = val / 10;
    /* Only handle up to 3 digits (sensors won't exceed 999 in tenths) */
    if (whole >= 100) { buf[pos++] = (char)('0' + whole / 100); }
    if (whole >= 10)  { buf[pos++] = (char)('0' + (whole / 10) % 10); }
    buf[pos++] = (char)('0' + whole % 10);
    buf[pos++] = '.';
    buf[pos++] = (char)('0' + val % 10);
    return pos;
}

/**
 * @brief Log current sensor values using manual char-by-char formatting.
 *        snprintf intentionally avoided — see file header for rationale.
 *        Format: "T:XX.X,XX.X,XX.X H:XX.X"
 */
static void Log_GreenhouseState(const Greenhouse_Sensors_t* data) {
    /* Static buffer keeps array off the task stack (defaultTask has 400B stack).
       Protected by the UART mutex to prevent interleaved output. */
    static char log_buffer[36];
    uint8_t pos = 0;

    /* "T:" temp1 "," temp2 "," temp3 " H:" humidity */
    log_buffer[pos++] = 'T'; log_buffer[pos++] = ':';
    pos = append_fixed(log_buffer, pos, data->temp1);
    log_buffer[pos++] = ',';
    pos = append_fixed(log_buffer, pos, data->temp2);
    log_buffer[pos++] = ',';
    pos = append_fixed(log_buffer, pos, data->temp3);
    log_buffer[pos++] = ' '; log_buffer[pos++] = 'H'; log_buffer[pos++] = ':';
    pos = append_fixed(log_buffer, pos, (int16_t)data->humidity);
    log_buffer[pos] = '\0';

    UART_Log_Lock();
    UART_Log_Direct(log_buffer);
    UART_Log_Unlock();
}
