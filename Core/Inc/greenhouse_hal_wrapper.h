/**
 ******************************************************************************
 * @file    greenhouse_hal_wrapper.h
 * @brief   Deklaracje warstwy abstrakcji sprzętu (HAL Wrapper) dla sterownika szklarni.
 *          Definiuje interfejsy dla czujników, aktuatorów oraz obsługi UART.
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
/* --- SEKCJA 1: UCHWYTY ZASOBÓW SYSTEMOWYCH ---                             */
/* ========================================================================== */

extern osMutexId_t uartMutexHandle;

/* ========================================================================== */
/* --- SEKCJA 2: INTERFEJSY ODCZYTU CZUJNIKÓW ---                             */
/* ========================================================================== */

// Odczyt temperatury z czujnika 1 (używany do sterowania piecem).
// Wartość w dziesiątych częściach stopnia Celsjusza (np. 245 = 24.5C).
int16_t GetTemp1(void);

// Odczyt temperatury z czujnika 2 (używany do liczenia średniej).
// Wartość w dziesiątych częściach stopnia Celsjusza.
int16_t GetTemp2(void);

// Odczyt temperatury z czujnika 3 (używany do sterowania klapami).
// Wartość w dziesiątych częściach stopnia Celsjusza.
int16_t GetTemp3(void);

// Odczyt wilgotności gleby/powietrza.
// Wartość w dziesiątych częściach procenta (np. 552 = 55.2%).
uint16_t GetHumidity(void);

/* ========================================================================== */
/* --- SEKCJA 3: STEROWANIE AKTUALTORAMI ---                                 */
/* ========================================================================== */

// Załączenie systemu ogrzewania (piec).
void HeaterOn(void);

// Wyłączenie systemu ogrzewania (piec).
void HeaterOff(void);

// Otwarcie klap wentylacyjnych.
void AirFlapsOpen(void);

// Zamknięcie klap wentylacyjnych.
void AirFlapsClose(void);

// Załączenie zraszaczy (podlewanie).
void WateringOn(void);

// Wyłączenie zraszaczy (podlewanie).
void WateringOff(void);

/* ========================================================================== */
/* --- SEKCJA 4: INTERFEJS UŻYTKOWNIKA (PRZYCISKI) ---                        */
/* ========================================================================== */

// Odczyt stanu fizycznego/symulowanego przycisku podlewania.
// 1 - wciśnięty, 0 - zwolniony.
uint8_t IsWateringButtonPressed(void);

/* ========================================================================== */
/* --- SEKCJA 5: OBSŁUGA UART (LOGOWANIE) ---                                */
/* ========================================================================== */

// Wysyła komunikat na port UART z automatyczną blokadą wątkową (mutex).
void UART_Log(const char* msg);

// Blokada mutexu portu UART (przydatne do składania złożonych linii).
void UART_Log_Lock(void);

// Zwolnienie mutexu portu UART.
void UART_Log_Unlock(void);

// Bezpośredni zapis na UART (wymaga wcześniejszej blokady mutexu).
void UART_Log_Direct(const char* msg);

/* ========================================================================== */
/* --- SEKCJA 6: SYMULATOR ---                                                */
/* ========================================================================== */

// Wykonuje krok symulatora środowiska czujników. Aktualizuje wbudowane
// stany symulowane pod kątem testów w terminalu.
void Greenhouse_HAL_SimulateStep(void);

#ifdef __cplusplus
}
#endif

#endif /* GREENHOUSE_HAL_WRAPPER_H */
