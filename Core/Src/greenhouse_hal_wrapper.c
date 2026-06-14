/**
 ******************************************************************************
 * @file    greenhouse_hal_wrapper.c
 * @brief   Implementacja warstwy abstrakcji sprzętu (HAL Wrapper).
 *          Zawiera generator stanów czujników (symulator) do testowania
 *          sterownika, definicje sterowania wyjściami oraz obsługę UART.
 *
 *          UWAGA: Świadomie zrezygnowano ze standardowego snprintf.
 *          Dla mikrokontrolera STM32F031K6 z 4KB RAM-u narzut stosu i pamięci FLASH
 *          ze strony biblioteki standardowej formatowania tekstu jest zbyt wysoki.
 ******************************************************************************
 */

#include "greenhouse_hal_wrapper.h"
#include "usart.h"
#include <string.h>

/* ========================================================================== */
/* --- SEKCJA 1: PRYWATNE ZMIENNE I SYMULOWANY STAN ---                      */
/* ========================================================================== */

// Bazowe warunki początkowe stabilnej szklarni (stałoprzecinkowo x10, np. 100 = 10.0C)
static int16_t sim_temp1   = 100;
static int16_t sim_temp2   = 100;
static int16_t sim_temp3   = 250;
static uint16_t sim_humidity = 550;
static uint8_t sim_button_pressed = 0;

// Licznik kroków symulacji środowiskowej do automatycznego przechodzenia faz testowych
static uint32_t sim_step = 0;

/* ========================================================================== */
/* --- SEKCJA 2: RĘCZNA KONWERSJA LICZB NA TEKST (BEZ SPRINTF) ---           */
/* ========================================================================== */

// Konwersja liczby całkowitej (uint32_t) na tekst.
// Zapisuje cyfry w odwrotnej kolejności, a na koniec odwraca bufor w miejscu.
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
        // Odwrócenie tekstu w buforze
        for (uint8_t i = 0, j = len - 1; i < j; i++, j--) {
            char tmp = buf[i]; buf[i] = buf[j]; buf[j] = tmp;
        }
    }
    return len;
}

// Konwersja wartości stałoprzecinkowej z jednym miejscem po przecinku na tekst (np. 245 -> "24.5").
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
/* --- SEKCJA 3: INTERFEJSY ODCZYTU CZUJNIKÓW (GETTERY) ---                  */
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
/* --- SEKCJA 4: STEROWANIE ELEMENTAMI WYKONAWCZYMI ---                      */
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
/* --- SEKCJA 5: ODCZYT WEJŚĆ UŻYTKOWNIKA ---                                */
/* ========================================================================== */

uint8_t IsWateringButtonPressed(void) {
    return sim_button_pressed;
}

/* ========================================================================== */
/* --- SEKCJA 6: KOMUNIKACJA SZEREGOWA I LOGOWANIE (UART) ---                */
/* ========================================================================== */

void UART_Log_Lock(void) {
    // Blokujemy mutex portu szeregowego przed wysyłaniem wiadomości, aby uniknąć nakładania się znaków
    if (uartMutexHandle != NULL) {
        osMutexAcquire(uartMutexHandle, osWaitForever);
    }
}

void UART_Log_Unlock(void) {
    // Zwalniamy mutex po zakończeniu transmisji linii
    if (uartMutexHandle != NULL) {
        osMutexRelease(uartMutexHandle);
    }
}

void UART_Log_Direct(const char* msg) {
    // Bezpośrednie wysłanie ciągu znaków. Mutex musi być już zablokowany przez wywołującego.
    HAL_UART_Transmit(&huart1, (uint8_t*)msg, (uint16_t)strlen(msg), HAL_MAX_DELAY);
    // Automatyczne dodanie przejścia do nowej linii na końcu każdego wpisu logów
    HAL_UART_Transmit(&huart1, (uint8_t*)"\r\n", 2, HAL_MAX_DELAY);
}

void UART_Log(const char* msg) {
    // Bezpieczna wielowątkowo funkcja logowania z automatyczną blokadą portu
    UART_Log_Lock();
    UART_Log_Direct(msg);
    UART_Log_Unlock();
}

/* ========================================================================== */
/* --- SEKCJA 7: GENERATOR SCENARIUSZY TESTOWYCH (SYMULATOR) ---            */
/* ========================================================================== */

// Symuluje powolną zmianę parametrów środowiskowych w czasie.
// Pozwala przetestować pełen cykl pracy sterownika (piec, klapy, podlewanie automatyczne
// oraz manualne) w oknie terminala bez dotykania fizycznego sprzętu.
void Greenhouse_HAL_SimulateStep(void) {
    sim_step++;

    /* FAZA 1: Stan stabilny (Baseline) - kroki 1-10 */
    if (sim_step <= 10) {
        sim_temp1 = 100; // 10.0C
        sim_temp2 = 100; // 10.0C
        sim_temp3 = 250; // 25.0C
        sim_humidity = 550; // 55.0%
        sim_button_pressed = 0;
    }
    /* FAZA 2: Spadek temperatury Temp1 - kroki 11-80
       - Od kroku 11 temperatura powoli spada o 0.6C na krok.
       - Po osiągnięciu 3.8C temperatura stabilizuje się, czekając na załączenie pieca.
       - Piec powinien włączyć się po 5 krokach od przekroczenia progu 4.0C (w kroku 25). */
    else if (sim_step > 10 && sim_step <= 20) {
        sim_temp1 -= 6;
    }
    else if (sim_step > 20 && sim_step <= 80) {
        sim_temp1 = 38; // Utrzymujemy stałe 3.8C
    }
    /* FAZA 3: Nagrzewanie szklarni i histereza ogrzewania - kroki 81-90
       - Temp1 rośnie o 0.8C na krok.
       - Po przekroczeniu progu wyłączenia >= 6.0C (w kroku 83), piec powinien się wyłączyć. */
    else if (sim_step > 80 && sim_step <= 90) {
        sim_temp1 += 8;
    }
    /* FAZA 4: Sytuacja alarmowa (Błyskawiczny mróz) - kroki 91-93
       - Temp1 spada skokowo do 0.5C.
       - Ogrzewanie powinno włączyć się natychmiast, ignorując opóźnienie 5-ciu cykli. */
    else if (sim_step > 90 && sim_step <= 93) {
        sim_temp1 = 5; // 0.5C
    }
    /* FAZA 5: Wygrzewanie alarmowe i powrót do normy - kroki 94-100 */
    else if (sim_step > 93 && sim_step <= 100) {
        sim_temp1 = 65; // 6.5C (piec wyłącza się)
    }
    /* FAZA 6: Alarm niskiej średniej temperatury (Zabezpieczenie upraw) - kroki 101-105
       - Temp1 = 6.0C (powyżej progu standardowego załączenia).
       - Temp2 i Temp3 spadają do 4.0C.
       - Średnia wynosi 4.6C (<= 5.0C), co powinno natychmiast wyzwolić piec. */
    else if (sim_step > 100 && sim_step <= 105) {
        sim_temp1 = 60;
        sim_temp2 = 40;
        sim_temp3 = 40;
    }
    /* FAZA 7: Powrót do stabilnych wartości bazowych - kroki 106-120 */
    else if (sim_step > 105 && sim_step <= 120) {
        sim_temp1 = 100;
        sim_temp2 = 100;
        sim_temp3 = 250;
    }
    /* FAZA 8: Wzrost temperatury Temp3 pod sterowanie klapami - kroki 121-250
       - Temp3 rośnie o 0.4C na krok i stabilizuje się na poziomie 28.5C (powyżej progu 28.0C).
       - Po 10 cyklach (w kroku 140) klapy powinny się otworzyć. */
    else if (sim_step > 120 && sim_step <= 130) {
        sim_temp3 += 4;
    }
    else if (sim_step > 130 && sim_step <= 250) {
        sim_temp3 = 285;
    }
    /* FAZA 9: Histereza klap (Ochłodzenie szklarni) - kroki 251-260
       - Temp3 spada do 23.5C. Klapy powinny zamknąć się natychmiast (próg to <= 24.0C). */
    else if (sim_step > 250 && sim_step <= 260) {
        sim_temp3 = 235;
    }
    /* FAZA 10: Spadek wilgotności gleby (Susza) - kroki 261-270
       - Wilgotność spada do 35.0% (< 40.0%). Zraszacze automatyczne powinny się włączyć. */
    else if (sim_step > 260 && sim_step <= 270) {
        sim_humidity = 350;
    }
    /* FAZA 11: Nawadnianie automatyczne (Histereza wilgotności) - kroki 271-280
       - Wilgotność rośnie do 62.0% (>= 60.0%). Zraszacze powinny się wyłączyć. */
    else if (sim_step > 270 && sim_step <= 280) {
        sim_humidity = 620;
    }
    /* FAZA 12: Test przycisku podlewania ręcznego - kroki 281+
       - W kroku 283 następuje naciśnięcie przycisku (sim_button_pressed = 1).
       - W kroku 284 przycisk zostaje zwolniony.
       - Zraszacze powinny się uruchomić na czas 10 sekund dzięki timerowi programowemu. */
    else if (sim_step == 283) {
        sim_button_pressed = 1;
    }
    else if (sim_step > 283) {
        sim_button_pressed = 0;
    }

    /* Ręczne złożenie logu symulacji w formacie: "S:Krok T1:X.Y T2:X.Y T3:X.Y H:X.Y B:Stan"
       Użycie bufora statycznego chroni wątek odczytu przed alokacją dużej tablicy na stosie. */
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
    HAL_UART_Transmit(&huart1, (uint8_t*)sim_log, pos, HAL_MAX_DELAY);
    HAL_UART_Transmit(&huart1, (uint8_t*)"\r\n", 2, HAL_MAX_DELAY);
    UART_Log_Unlock();
}
