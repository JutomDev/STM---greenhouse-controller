/**
  ******************************************************************************
  * @file    greenhouse_hal_wrapper.c
  * @brief   Hardware Abstraction Layer (HAL) wrapper implementation.
  *          Provides mock sensor readings in fixed-point integer format,
  *          actuator controls, and logs state changes to UART with Mutex protection.
  *
  *          IMPORTANT: snprintf is intentionally NOT used anywhere in this file.
  *          On Cortex-M0 with newlib-nano, snprintf pulls in ~3KB of FLASH and
  *          requires 200-300B of stack per call. With a 4KB RAM budget, that is
  *          fatal. All formatting uses manual helpers below.
  ******************************************************************************
  */

#include "greenhouse_hal_wrapper.h"
#include "usart.h"
#include <string.h>

/* --- Private Variables (Simulated State) --- */

/* Initial baseline conditions for a stable greenhouse environment.
   Values are stored in fixed-point representation (tenths of units)
   to completely avoid floating-point calculations and formatting. */
static int16_t sim_temp1   = 100;
static int16_t sim_temp2   = 100;
static int16_t sim_temp3   = 250;
static uint16_t sim_humidity = 550;
static uint8_t sim_button_pressed = 0;

/* Internal simulation counter to drive the test scenarios step-by-step */
static uint32_t sim_step = 0;

/* --- Private Helper: integer-to-string conversion (no snprintf) --- */

/**
 * @brief Write an unsigned 32-bit integer as ASCII decimal into buf.
 *        Returns number of characters written (no null terminator).
 *        Avoids snprintf entirely to keep stack and FLASH usage minimal.
 */
static uint8_t u32_to_str(char *buf, uint32_t val)
{
    /* Write digits in reverse, then flip the string in-place */
    uint8_t len = 0;
    if (val == 0) {
        buf[len++] = '0';
    } else {
        while (val > 0) {
            buf[len++] = (char)('0' + (val % 10));
            val /= 10;
        }
        /* Reverse digits */
        for (uint8_t i = 0, j = len - 1; i < j; i++, j--) {
            char tmp = buf[i]; buf[i] = buf[j]; buf[j] = tmp;
        }
    }
    return len;
}

/**
 * @brief Write a signed 16-bit integer divided by 10 as "X.Y" decimal (one decimal place).
 *        Used for fixed-point sensor values (e.g. 245 → "24.5").
 *        Returns number of characters written.
 */
static uint8_t fixed_to_str(char *buf, int16_t val)
{
    uint8_t len = 0;
    if (val < 0) {
        buf[len++] = '-';
        val = (int16_t)(-val);
    }
    len += u32_to_str(&buf[len], (uint32_t)(val / 10));
    buf[len++] = '.';
    buf[len++] = (char)('0' + (val % 10));
    return len;
}

/* --- Sensor Readings --- */

int16_t GetTemp1(void) {
    return sim_temp1;
}

int16_t GetTemp2(void) {
    return sim_temp2;
}

int16_t GetTemp3(void) {
    return sim_temp3;
}

uint16_t GetHumidity(void) {
    return sim_humidity;
}

/* --- Actuator Controls --- */

void HeaterOn(void) {
    UART_Log(">>> ACTUATOR: Heater ON");
}

void HeaterOff(void) {
    UART_Log(">>> ACTUATOR: Heater OFF");
}

void AirFlapsOpen(void) {
    UART_Log(">>> ACTUATOR: Flaps OPEN");
}

void AirFlapsClose(void) {
    UART_Log(">>> ACTUATOR: Flaps CLOSE");
}

void WateringOn(void) {
    UART_Log(">>> ACTUATOR: Sprinklers ON");
}

void WateringOff(void) {
    UART_Log(">>> ACTUATOR: Sprinklers OFF");
}

/* --- User Inputs --- */

uint8_t IsWateringButtonPressed(void) {
    return sim_button_pressed;
}

/* --- Communication / Logging --- */

void UART_Log_Lock(void) {
    /* If the scheduler is running, block on acquiring the UART Mutex.
       We use osWaitForever to ensure we eventually get access to serialize prints. */
    if (uartMutexHandle != NULL) {
        osMutexAcquire(uartMutexHandle, osWaitForever);
    }
}

void UART_Log_Unlock(void) {
    /* Release the UART Mutex after printing is complete */
    if (uartMutexHandle != NULL) {
        osMutexRelease(uartMutexHandle);
    }
}

void UART_Log_Direct(const char* msg) {
    /* Direct write to UART. Caller must guarantee thread-safety. */
    HAL_UART_Transmit(&huart1, (uint8_t*)msg, (uint16_t)strlen(msg), HAL_MAX_DELAY);
    /* Always append CR+LF — no need for the caller to include them */
    HAL_UART_Transmit(&huart1, (uint8_t*)"\r\n", 2, HAL_MAX_DELAY);
}

void UART_Log(const char* msg) {
    /* Thread-safe wrapper for printing standard string messages */
    UART_Log_Lock();
    UART_Log_Direct(msg);
    UART_Log_Unlock();
}

/* --- Simulation Control Scenario --- */

void Greenhouse_HAL_SimulateStep(void) {
    sim_step++;

    /* PHASE 1: Stable baseline (Steps 1-10) */
    if (sim_step <= 10) {
        sim_temp1 = 100;
        sim_temp2 = 100;
        sim_temp3 = 250;
        sim_humidity = 550;
        sim_button_pressed = 0;
    }
    /* PHASE 2: Temp1 drops below 4.0C (Steps 11-80)
       We hold it at 3.8C (38) to test that the heater starts exactly after 5 reading intervals.
       - Temperature drops below 4.0C at step 20.
       - Heater should turn ON after 5 more steps (step 25). */
    else if (sim_step > 10 && sim_step <= 20) {
        sim_temp1 -= 6; /* Drops by 0.6C per step */
    }
    else if (sim_step > 20 && sim_step <= 80) {
        sim_temp1 = 38; /* Held constant at 3.8C */
    }
    /* PHASE 3: Temp1 rises back to test heater hysteresis (Steps 81-90)
       - Temperature reaches >= 6.0C (60) at step 83.
       - Heater should turn OFF. */
    else if (sim_step > 80 && sim_step <= 90) {
        sim_temp1 += 8; /* Rises by 0.8C per step */
    }
    /* PHASE 4: Critical low temperature exception on Temp1 (Steps 91-93)
       - Temp1 drops instantly to 0.5C (5) (<= 1.0C).
       - Heater must turn ON immediately (no delay). */
    else if (sim_step > 90 && sim_step <= 93) {
        sim_temp1 = 5; /* 0.5C */
    }
    /* PHASE 5: Heater warms up after critical exception (Steps 94-100) */
    else if (sim_step > 93 && sim_step <= 100) {
        sim_temp1 = 65; /* 6.5C */
    }
    /* PHASE 6: Critical average temperature exception (Steps 101-105)
       - Temp1 = 6.0C (60) (normally wouldn't trigger heater).
       - Temp2 = 4.0C (40), Temp3 = 4.0C (40).
       - Average = (60 + 40 + 40) / 3 = 46 (4.6C) (<= 5.0C).
       - Heater must turn ON immediately. */
    else if (sim_step > 100 && sim_step <= 105) {
        sim_temp1 = 60;
        sim_temp2 = 40;
        sim_temp3 = 40;
    }
    /* PHASE 7: Temp recover to normal (Steps 106-120) */
    else if (sim_step > 105 && sim_step <= 120) {
        sim_temp1 = 100;
        sim_temp2 = 100;
        sim_temp3 = 250;
    }
    /* PHASE 8: Temp3 rises above 28.0C to test air flaps (Steps 121-250)
       - Temp3 rises above 28.0C (280) at step 130.
       - Flaps should open after 10 more steps (step 140). */
    else if (sim_step > 120 && sim_step <= 130) {
        sim_temp3 += 4; /* Rises by 0.4C per step */
    }
    else if (sim_step > 130 && sim_step <= 250) {
        sim_temp3 = 285; /* 28.5C */
    }
    /* PHASE 9: Temp3 drops below 24.0C to test closing flaps (Steps 251-260)
       - Temp3 drops to 23.5C (235). Flaps should close immediately. */
    else if (sim_step > 250 && sim_step <= 260) {
        sim_temp3 = 235;
    }
    /* PHASE 10: Humidity drops below 40% to test AUTO watering (Steps 261-270)
       - Humidity drops to 35.0% (350). Sprinklers should turn ON. */
    else if (sim_step > 260 && sim_step <= 270) {
        sim_humidity = 350;
    }
    /* PHASE 11: Humidity rises above 60% to test AUTO watering stop (Steps 271-280)
       - Humidity rises to 62.0% (620). Sprinklers should turn OFF. */
    else if (sim_step > 270 && sim_step <= 280) {
        sim_humidity = 620;
    }
    /* PHASE 12: Manual button press simulation (Steps 281-290)
       - Button is pressed at step 283 (sim_button_pressed = 1).
       - Button released at step 284.
       - Sprinklers should turn ON. */
    else if (sim_step == 283) {
        sim_button_pressed = 1;
    }
    else if (sim_step > 283) {
        sim_button_pressed = 0; /* Released */
    }

    /* Build the simulation log line manually — NO snprintf.
       Format: "S:NNNN T1:XX.X T2:XX.X T3:XX.X H:XX.X B:N"
       Static buffer to avoid stack allocation of a large array on readTask's tiny stack. */
    static char sim_log[52];
    uint8_t pos = 0;

    /* "S:" + step counter */
    sim_log[pos++] = 'S'; sim_log[pos++] = ':';
    pos += u32_to_str(&sim_log[pos], sim_step);

    /* " T1:" + temp1 */
    sim_log[pos++] = ' '; sim_log[pos++] = 'T'; sim_log[pos++] = '1'; sim_log[pos++] = ':';
    pos += fixed_to_str(&sim_log[pos], sim_temp1);

    /* " T2:" + temp2 */
    sim_log[pos++] = ' '; sim_log[pos++] = 'T'; sim_log[pos++] = '2'; sim_log[pos++] = ':';
    pos += fixed_to_str(&sim_log[pos], sim_temp2);

    /* " T3:" + temp3 */
    sim_log[pos++] = ' '; sim_log[pos++] = 'T'; sim_log[pos++] = '3'; sim_log[pos++] = ':';
    pos += fixed_to_str(&sim_log[pos], sim_temp3);

    /* " H:" + humidity */
    sim_log[pos++] = ' '; sim_log[pos++] = 'H'; sim_log[pos++] = ':';
    pos += fixed_to_str(&sim_log[pos], (int16_t)sim_humidity);

    /* " B:" + button */
    sim_log[pos++] = ' '; sim_log[pos++] = 'B'; sim_log[pos++] = ':';
    sim_log[pos++] = (char)('0' + sim_button_pressed);

    /* Null-terminate for strlen in UART_Log_Direct */
    sim_log[pos] = '\0';

    UART_Log_Lock();
    HAL_UART_Transmit(&huart1, (uint8_t*)sim_log, pos, HAL_MAX_DELAY);
    HAL_UART_Transmit(&huart1, (uint8_t*)"\r\n", 2, HAL_MAX_DELAY);
    UART_Log_Unlock();
}
