/**
 ******************************************************************************
 * @file    greenhouse_logic.c
 * @brief   Główna logika biznesowa i implementacja zadań FreeRTOS dla sterownika szklarni.
 *
 *          UWAGA: Świadomie nie używamy funkcji typu sprintf/snprintf. Na
 *          rdzeniu Cortex-M0 biblioteka newlib-nano zużywa przez to około 3KB FLASH i
 *          ponad 200 bajtów stosu na każde wywołanie. Przy 4KB RAM-u skończyłoby się to
 *          przepełnieniem stosu i błędem HardFault. Wszystkie napisy są składane ręcznie.
 ******************************************************************************
 */

#include "greenhouse_logic.h"
#include "FreeRTOS.h"
#include "greenhouse_hal_wrapper.h"
#include "task.h"
#include <string.h>

/* ========================================================================== */
/* --- SEKCJA 1: DEFINICJE I ZMIENNE STANU ---                       */
/* ========================================================================== */

// Czas trwania podlewania ręcznego (10 sekund na potrzeby szybkiej
// demonstracji)
#define MANUAL_WATERING_DURATION_MS (10 * 1000)

typedef enum { HEATER_STATE_OFF = 0, HEATER_STATE_ON } HeaterState_t;

typedef enum { FLAPS_STATE_CLOSED = 0, FLAPS_STATE_OPEN } FlapsState_t;

// Flagi podlewania oznaczone jako volatile, bo są zmieniane zarówno w zadaniu
// głównym, jak i w timerze
static volatile uint8_t auto_watering_active = 0;
static volatile uint8_t manual_watering_active = 0;

// Stany wykonawcze przekazywane do timera LED
static volatile uint8_t current_heater_state = 0;
static volatile uint8_t current_flaps_state = 0;

/* ========================================================================== */
/* --- SEKCJA 2: PROTOTYPY FUNKCJI PRYWATNYCH ---                            */
/* ========================================================================== */

// Aktualizuje fizyczny stan przekaźnika zraszania po zsumowaniu warunków AUTO i
// MANUAL
static void Greenhouse_Logic_UpdateWateringState(void);

// Ręczne formatowanie stanu szklarni do wysłania przez UART (zamiast snprintf)
static void Log_GreenhouseState(const Greenhouse_Sensors_t *data);

/* ========================================================================== */
/* --- SEKCJA 3: FUNKCJE DOSTĘPOWE (GETTERY) ---                              */
/* ========================================================================== */

uint8_t Greenhouse_Logic_GetHeaterState(void) { return current_heater_state; }

uint8_t Greenhouse_Logic_GetFlapsState(void) { return current_flaps_state; }

uint8_t Greenhouse_Logic_GetWateringState(void) {
  return (auto_watering_active || manual_watering_active);
}

/* ========================================================================== */
/* --- SEKCJA 4: ZADANIA FREERTOS (TASKS) ---                                 */
/* ========================================================================== */

// Zadanie cyklicznego odpytywania czujników (częstotliwość GREENHOUSE_READ_INTERVAL_MS).
// Pobiera pomiary z warstwy HAL i przesyła je kolejką do zadania logicznego.
void Task_Read_Rtn(void *argument) {
  (void)argument;
  UART_Log("DBG: readTask started");
  Greenhouse_Sensors_t sensor_data;

  for (;;) {
    // Krok symulacji środowiska (w produkcji: rzeczywisty odczyt ADC/I2C)
    Greenhouse_HAL_SimulateStep();

    sensor_data.temp1 = GetTemp1();
    sensor_data.temp2 = GetTemp2();
    sensor_data.temp3 = GetTemp3();
    sensor_data.humidity = GetHumidity();

    // Przesłanie danych do kolejki z blokowaniem wątku w przypadku zapełnienia
    osStatus_t status = osMessageQueuePut(greenhouseQueueHandle, &sensor_data,
                                          0, osWaitForever);
    if (status != osOK) {
      UART_Log("ERROR: Failed to push sensor data to queue!");
    }

    // Uśpienie wątku na czas interwału próbkowania (oszczędność energii)
    osDelay(pdMS_TO_TICKS(GREENHOUSE_READ_INTERVAL_MS));
  }
}

// Zadanie nadrzędne (Logiczne). Odbiera pomiary z kolejki, analizuje
// warunki środowiskowe oraz asynchronicznie reaguje na semafor od przycisku
// podlewania.
void Task_Logic_Rtn(void *argument) {
  (void)argument;
  UART_Log("DBG: defaultTask started");
  Greenhouse_Sensors_t data;

  HeaterState_t heater_state = HEATER_STATE_OFF;
  FlapsState_t flaps_state = FLAPS_STATE_CLOSED;

  uint32_t temp1_low_minutes = 0;
  uint32_t temp3_high_minutes = 0;

  for (;;) {
    // Czekamy na dane z kolejki z timeoutem 100ms.
    // Dzięki temu nie blokujemy pętli na stałe i możemy regularnie sprawdzać
    // semafor przycisku.
    osStatus_t status = osMessageQueueGet(greenhouseQueueHandle, &data, NULL,
                                          pdMS_TO_TICKS(100));

    if (status == osOK) {
      // Wypisanie odebranych danych na port szeregowy
      Log_GreenhouseState(&data);

      /* --- 1. SYSTEM OGRZEWANIA (Heater) --- */
      // Średnia temperatura z 3 punktów pomiarowych (stałoprzecinkowo)
      int16_t avg_temp = (data.temp1 + data.temp2 + data.temp3) / 3;

      if (heater_state == HEATER_STATE_OFF) {
        // Sytuacja krytyczna: natychmiastowe uruchomienie ogrzewania bez
        // opóźnienia 10 = 1.0C, 50 = 5.0C
        if (data.temp1 <= 10 || avg_temp <= 50) {
          heater_state = HEATER_STATE_ON;
          current_heater_state = 1;
          HeaterOn();
          UART_Log("ALARM: Critical temp! Heater ON!");
        }
        // Warunek standardowy: temperatura poniżej 4.0C przez 5 minut (5 cykli
        // odczytu)
        else if (data.temp1 < 40) {
          temp1_low_minutes++;
          if (temp1_low_minutes >= 5) {
            heater_state = HEATER_STATE_ON;
            current_heater_state = 1;
            HeaterOn();
            UART_Log("SYSTEM: Temp1 low 5m. Heater ON.");
          }
        }
        // Reset licznika czasu w przypadku poprawy temperatury przed
        // załączeniem pieca
        else {
          temp1_low_minutes = 0;
        }
      } else { // heater_state == HEATER_STATE_ON
        // Wyłączenie ogrzewania po osiągnięciu 6.0C (histereza zapobiegająca
        // oscylacji przekaźnika)
        if (data.temp1 >= 60) {
          heater_state = HEATER_STATE_OFF;
          current_heater_state = 0;
          HeaterOff();
          temp1_low_minutes = 0;
          UART_Log("SYSTEM: Target reached. Heater OFF.");
        }
      }

      /* --- 2. KLAPY WENTYLACYJNE (Flaps) --- */
      if (flaps_state == FLAPS_STATE_CLOSED) {
        // Otwieramy klapy, gdy Temp3 przekracza 28.0C przez 10 minut (10 cykli
        // odczytu)
        if (data.temp3 > 280) {
          temp3_high_minutes++;
          if (temp3_high_minutes >= 10) {
            flaps_state = FLAPS_STATE_OPEN;
            current_flaps_state = 1;
            AirFlapsOpen();
            UART_Log("SYSTEM: Temp3 high 10m. Flaps OPEN.");
          }
        }
        // Anulowanie odliczania czasu, gdy temperatura spadnie w trakcie
        // naliczania
        else {
          temp3_high_minutes = 0;
        }
      } else { // flaps_state == FLAPS_STATE_OPEN
        // Natychmiastowe zamkniecie klap przy spadku do 24.0C, by chronić
        // uprawy przed wychłodzeniem
        if (data.temp3 <= 240) {
          flaps_state = FLAPS_STATE_CLOSED;
          current_flaps_state = 0;
          AirFlapsClose();
          temp3_high_minutes = 0;
          UART_Log("SYSTEM: Temp3 cooled. Flaps CLOSE.");
        }
      }

      /* --- 3. AUTOMATYCZNE PODLEWANIE (AUTO Watering) --- */
      if (!auto_watering_active) {
        // Rozpocznij zraszanie, gdy wilgotność gleby spadnie poniżej 40.0%
        if (data.humidity < 400) {
          auto_watering_active = 1;
          UART_Log("SYSTEM: Humidity <40%. Sprinklers ON.");
          Greenhouse_Logic_UpdateWateringState();
        }
      } else {
        // Wyłącz zraszanie po nawodnieniu gleby do poziomu 60.0%
        if (data.humidity >= 600) {
          auto_watering_active = 0;
          UART_Log("SYSTEM: Humidity >=60%. Sprinklers OFF.");
          Greenhouse_Logic_UpdateWateringState();
        }
      }
    }

    /* --- 4. OBSŁUGA PODLEWANIA RĘCZNEGO --- */
    // Sprawdzamy semafor binarny powiązany z przerwaniem przycisku
    // (nieblokująco)
    if (buttonSemaphoreHandle != NULL) {
      if (osSemaphoreAcquire(buttonSemaphoreHandle, 0) == osOK) {
        UART_Log("USER: Manual button interrupt detected.");

        // Włączenie zraszaczy z poziomu żądania użytkownika
        manual_watering_active = 1;
        Greenhouse_Logic_UpdateWateringState();

        // Uruchomienie lub zresetowanie 10-sekundowego timera systemowego typu
        // one-shot
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
/* --- SEKCJA 5: CALLBACKI TIMERÓW SYSTEMOWYCH ---                            */
/* ========================================================================== */

// Callback timera podlewania ręcznego. Wywoływany automatycznie po 10
// sekundach.
void ManualWateringTimer_Callback(void *argument) {
  (void)argument;
  UART_Log("SYSTEM: Manual timer expired.");

  // Koniec cyklu podlewania manualnego
  manual_watering_active = 0;
  Greenhouse_Logic_UpdateWateringState();
}

// Cykliczny callback (100ms) odpowiedzialny za sterowanie diodą LD3
// (sygnalizacja stanu).
void LEDTimer_Callback(void *argument) {
  (void)argument;
  static uint32_t ticks = 0;
  ticks++;

  uint8_t heater = Greenhouse_Logic_GetHeaterState();
  uint8_t watering = Greenhouse_Logic_GetWateringState();
  uint8_t flaps = Greenhouse_Logic_GetFlapsState();

  if (watering) {
    // Światło ciągłe – zraszacze pracują (najwyższy priorytet)
    HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_SET);
  } else if (heater) {
    // Szybkie miganie (5 Hz, zmiana stanu co 100ms) – ogrzewanie włączone
    HAL_GPIO_TogglePin(LD3_GPIO_Port, LD3_Pin);
  } else if (flaps) {
    // Wolne miganie (1 Hz, zmiana stanu co 500ms / 5 ticks) – klapy otwarte
    if (ticks % 5 == 0) {
      HAL_GPIO_TogglePin(LD3_GPIO_Port, LD3_Pin);
    }
  } else {
    // Tryb czuwania (krótki błysk co 2 sekundy / 20 ticks) – system żyje, brak
    // aktywności
    if (ticks % 20 == 0) {
      HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_SET);
    } else if (ticks % 20 == 1) {
      HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);
    }
  }
}

/* ========================================================================== */
/* --- SEKCJA 6: OBSŁUGA PRZERWAŃ (EXTI) ---                                  */
/* ========================================================================== */

// Callback wywoływany przez sprzętowe przerwanie linii EXTI GPIO (zbocze
// opadające na PA0).
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
  if (GPIO_Pin == BUTTON_Pin) {
    // Zwalniamy semafor, aby obudzić logikę w defaultTask.
    // Interfejs CMSIS-RTOS V2 bezpiecznie radzi sobie z wywoływaniem
    // osSemaphoreRelease w kontekście ISR.
    if (buttonSemaphoreHandle != NULL) {
      osSemaphoreRelease(buttonSemaphoreHandle);
    }
  }
}

/* ========================================================================== */
/* --- SEKCJA 7: FUNKCJE POMOCNICZE I FORMATOWANIE ---                        */
/* ========================================================================== */

// Bezpieczna, odporna na wyścigi (sekcja krytyczna) aktualizacja stanu zraszacza.
static void Greenhouse_Logic_UpdateWateringState(void) {
  static uint8_t last_watering_state = 0;
  uint8_t should_be_on = 0;
  uint8_t state_changed = 0;

  // Sekcja krytyczna chroni flagi przed jednoczesnym zapisem z przerwawnia i
  // callbacka
  taskENTER_CRITICAL();
  should_be_on = (auto_watering_active || manual_watering_active);
  if (should_be_on != last_watering_state) {
    last_watering_state = should_be_on;
    state_changed = 1;
  }
  taskEXIT_CRITICAL();

  // Wywołanie fizycznego zapisu na pin i logowania poza sekcją krytyczną
  if (state_changed) {
    if (should_be_on) {
      WateringOn();
    } else {
      WateringOff();
    }
  }
}

// Ręczne wpisanie liczby stałoprzecinkowej do bufora tekstowego (np. 245 ->
// "24.5")
static uint8_t append_fixed(char *buf, uint8_t pos, int16_t val) {
  if (val < 0) {
    buf[pos++] = '-';
    val = (int16_t)(-val);
  }
  int16_t whole = val / 10;

  // Maksymalnie obsługujemy 3 cyfry przed przecinkiem (zakres do 99.9)
  if (whole >= 100) {
    buf[pos++] = (char)('0' + whole / 100);
  }
  if (whole >= 10) {
    buf[pos++] = (char)('0' + (whole / 10) % 10);
  }
  buf[pos++] = (char)('0' + whole % 10);
  buf[pos++] = '.';
  buf[pos++] = (char)('0' + val % 10);
  return pos;
}

// Składa raport środowiskowy bez użycia sprintf i wypisuje go na port UART
static void Log_GreenhouseState(const Greenhouse_Sensors_t *data) {
  // Statyczny bufor zapobiega alokacji dużych tablic na małym stosie zadania
  // (400B)
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

  // Mutex zabezpiecza port szeregowy przed jednoczesnym pisaniem z różnych
  // wątków
  UART_Log_Lock();
  UART_Log_Direct(log_buffer);
  UART_Log_Unlock();
}
