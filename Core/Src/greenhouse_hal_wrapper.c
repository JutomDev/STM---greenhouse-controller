/**
 ******************************************************************************
 * @file    greenhouse_hal_wrapper.c
 * @brief   Hardware Abstraction Layer implementation for greenhouse controller.
 *          Provides sensor getters, actuator drivers, UART logging, and an
 *          in-memory environmental state generator for hardware-in-the-loop testing.
 *
 *          Design Note: Standard library printf/snprintf is deliberately avoided.
 *          On ARM Cortex-M0 with 4 KB SRAM, newlib-nano snprintf imposes an
 *          intolerable ~3 KB Flash footprint and over 200 bytes of stack consumption
 *          per call frame, risking HardFault stack collisions. Numeric formatting is
 *          performed using deterministic, minimal-stack fixed-point routines.
 ******************************************************************************
 */

#include "greenhouse_hal_wrapper.h"
#include "usart.h"
#include <string.h>

/* ========================================================================== */
/* --- PRIVATE SIMULATION STATE & SENSOR REGISTERS -------------------------- */
/* ========================================================================== */

/* Baseline environmental values in fixed-point deci-units (e.g., 100 = 10.0 deg C) */
static int16_t sim_temp1   = 100;
static int16_t sim_temp2   = 100;
static int16_t sim_temp3   = 250;
static uint16_t sim_humidity = 550;
static uint8_t sim_button_pressed = 0;

/* Monotonic simulation step counter driving automated scenario progression */
static uint32_t sim_step = 0;

/* Bounded timeout (ms) for UART polling transmit to prevent thread lockup if TX line hangs */
#define UART_TX_TIMEOUT_MS  100

/* ========================================================================== */
/* --- INTEGER FORMATTING ROUTINES (STACK-OPTIMIZED ALTERNATIVE TO SPRINTF) - */
/* ========================================================================== */

/**
 * @brief Converts unsigned 32-bit integer to ASCII decimal string in-place.
 * @note  Operates with zero dynamic memory allocation and O(log10(N)) stack depth.
 */
static uint8_t u32_to_str(char *buf, uint32_t val)
{
    uint8_t len = 0;
    if (val == 0) {
        buf[len++] = '0';
    } else {
        while (val > 0) {
            buf[len++] = (char)('0' + (val % 10));
            val /= 10;
        }
        /* Invert buffer in-place to correct digit order */
        for (uint8_t i = 0, j = len - 1; i < j; i++, j--) {
            char tmp = buf[i]; buf[i] = buf[j]; buf[j] = tmp;
        }
    }
    return len;
}

/**
 * @brief Converts fixed-point signed integer (1 decimal place) to ASCII string (e.g. 245 -> "24.5").
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

/* ========================================================================== */
/* --- SENSOR TELEMETRY GETTERS --------------------------------------------- */
/* ========================================================================== */

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

/* ========================================================================== */
/* --- ACTUATOR CONTROL INTERFACES ------------------------------------------ */
/* ========================================================================== */

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

/* ========================================================================== */
/* --- USER INPUT QUERY ----------------------------------------------------- */
/* ========================================================================== */

uint8_t IsWateringButtonPressed(void) {
    return sim_button_pressed;
}

/* ========================================================================== */
/* --- SERIAL LOGGING & THREAD-SAFE CONSOLE (USART1) ------------------------ */
/* ========================================================================== */

void UART_Log_Lock(void) {
    /* Guard serial output against interleaved lines from concurrent RTOS tasks */
    if (uartMutexHandle != NULL) {
        osMutexAcquire(uartMutexHandle, osWaitForever);
    }
}

void UART_Log_Unlock(void) {
    if (uartMutexHandle != NULL) {
        osMutexRelease(uartMutexHandle);
    }
}

void UART_Log_Direct(const char* msg) {
    if (msg == NULL) {
        return;
    }
    /* Transmit payload using bounded timeout to avoid permanent hang if hardware handshake stalls */
    HAL_UART_Transmit(&huart1, (uint8_t*)msg, (uint16_t)strlen(msg), UART_TX_TIMEOUT_MS);
    HAL_UART_Transmit(&huart1, (uint8_t*)"\r\n", 2, UART_TX_TIMEOUT_MS);
}

void UART_Log(const char* msg) {
    if (msg == NULL) {
        return;
    }
    UART_Log_Lock();
    UART_Log_Direct(msg);
    UART_Log_Unlock();
}

/* ========================================================================== */
/* --- TESTBENCH ENVIRONMENTAL PROFILE GENERATOR ---------------------------- */
/* ========================================================================== */

/**
 * @brief Advances deterministic climate profile across simulation time steps.
 *        Emulates thermal inertia, moisture absorption, and threshold crossings
 *        for validation without physical sensor rigs.
 */
void Greenhouse_HAL_SimulateStep(void) {
    sim_step++;

    /* Phase 1: Nominal baseline equilibrium (Steps 1-10) */
    if (sim_step <= 10) {
        sim_temp1 = 100;    /* 10.0 deg C */
        sim_temp2 = 100;    /* 10.0 deg C */
        sim_temp3 = 250;    /* 25.0 deg C */
        sim_humidity = 550; /* 55.0% RH   */
        sim_button_pressed = 0;
    }
    /* Phase 2: Gradual ambient cooling - Temp1 drops below 4.0 deg C (Steps 11-80)
     * Decrements by 0.6 deg C per step, settling at 3.8 deg C.
     * Triggers heater after 5 continuous sample cycles below 4.0 deg C (Step 25).
     */
    else if (sim_step > 10 && sim_step <= 20) {
        sim_temp1 -= 6;
    }
    else if (sim_step > 20 && sim_step <= 80) {
        sim_temp1 = 38;     /* Steady 3.8 deg C */
    }
    /* Phase 3: Heating response & cut-off hysteresis (Steps 81-90)
     * Temp1 rises until reaching cut-off threshold (>= 6.0 deg C), de-energizing heater.
     */
    else if (sim_step > 80 && sim_step <= 90) {
        sim_temp1 += 8;
    }
    /* Phase 4: Emergency frost condition (Steps 91-93)
     * Rapid drop to 0.5 deg C (< 1.0 deg C safety floor) triggers immediate heater override.
     */
    else if (sim_step > 90 && sim_step <= 93) {
        sim_temp1 = 5;      /* 0.5 deg C */
    }
    /* Phase 5: Thermal recovery from frost alarm (Steps 94-100) */
    else if (sim_step > 93 && sim_step <= 100) {
        sim_temp1 = 65;     /* 6.5 deg C */
    }
    /* Phase 6: Spatial gradient alarm (Steps 101-105)
     * Average canopy temperature <= 5.0 deg C triggers immediate heater override.
     */
    else if (sim_step > 100 && sim_step <= 105) {
        sim_temp1 = 60;
        sim_temp2 = 40;
        sim_temp3 = 40;     /* Mean = 4.6 deg C */
    }
    /* Phase 7: Post-alarm stabilization (Steps 106-120) */
    else if (sim_step > 105 && sim_step <= 120) {
        sim_temp1 = 100;
        sim_temp2 = 100;
        sim_temp3 = 250;
    }
    /* Phase 8: Ridge solar gain - Temp3 > 28.0 deg C for 10 cycles (Steps 121-250)
     * Opens ventilation flaps at Step 140 after 10 continuous warm cycles.
     */
    else if (sim_step > 120 && sim_step <= 130) {
        sim_temp3 += 4;
    }
    else if (sim_step > 130 && sim_step <= 250) {
        sim_temp3 = 285;    /* 28.5 deg C */
    }
    /* Phase 9: Ventilation cooling hysteresis (Steps 251-260)
     * Flaps close immediately when Temp3 falls back to <= 24.0 deg C.
     */
    else if (sim_step > 250 && sim_step <= 260) {
        sim_temp3 = 235;    /* 23.5 deg C */
    }
    /* Phase 10: Soil moisture depletion (Steps 261-270)
     * Humidity drops to 35.0% (< 40.0% threshold), activating automated irrigation.
     */
    else if (sim_step > 260 && sim_step <= 270) {
        sim_humidity = 350;
    }
    /* Phase 11: Moisture saturation hysteresis (Steps 271-280)
     * Humidity recovers to 62.0% (>= 60.0% threshold), shutting off sprinklers.
     */
    else if (sim_step > 270 && sim_step <= 280) {
        sim_humidity = 620;
    }
    /* Phase 12: Manual operator override event (Steps 281+)
     * Pulse button state at step 283 to test external interrupt ISR queueing.
     */
    else if (sim_step == 283) {
        sim_button_pressed = 1;
    }
    else if (sim_step > 283) {
        sim_button_pressed = 0;
    }

    /* Assemble fixed-width telemetry line: "S:Step T1:X.Y T2:X.Y T3:X.Y H:X.Y B:State"
     * Static buffer prevents allocating 52 bytes on caller task stack frame.
     */
    static char sim_log[52];
    uint8_t pos = 0;

    sim_log[pos++] = 'S'; sim_log[pos++] = ':';
    pos += u32_to_str(&sim_log[pos], sim_step);

    sim_log[pos++] = ' '; sim_log[pos++] = 'T'; sim_log[pos++] = '1'; sim_log[pos++] = ':';
    pos += fixed_to_str(&sim_log[pos], sim_temp1);

    sim_log[pos++] = ' '; sim_log[pos++] = 'T'; sim_log[pos++] = '2'; sim_log[pos++] = ':';
    pos += fixed_to_str(&sim_log[pos], sim_temp2);

    sim_log[pos++] = ' '; sim_log[pos++] = 'T'; sim_log[pos++] = '3'; sim_log[pos++] = ':';
    pos += fixed_to_str(&sim_log[pos], sim_temp3);

    sim_log[pos++] = ' '; sim_log[pos++] = 'H'; sim_log[pos++] = ':';
    pos += fixed_to_str(&sim_log[pos], (int16_t)sim_humidity);

    sim_log[pos++] = ' '; sim_log[pos++] = 'B'; sim_log[pos++] = ':';
    sim_log[pos++] = (char)('0' + sim_button_pressed);

    sim_log[pos] = '\0';

    UART_Log_Lock();
    HAL_UART_Transmit(&huart1, (uint8_t*)sim_log, pos, UART_TX_TIMEOUT_MS);
    HAL_UART_Transmit(&huart1, (uint8_t*)"\r\n", 2, UART_TX_TIMEOUT_MS);
    UART_Log_Unlock();
}

