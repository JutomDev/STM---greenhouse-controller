/**
 ******************************************************************************
 * @file    greenhouse_logic.h
 * @brief   Deklaracje głównej logiki biznesowej, struktur danych oraz punktów
 *          wejściowych zadań FreeRTOS sterownika szklarni.
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
/* --- SEKCJA 1: USTAWIENIA CZASOWE I KONFIGURACJA (TEST vs PRODUKCJA) ---    */
/* ========================================================================== */

// Częstotliwość próbkowania czujników (Task_Read)
// [TRYB TESTOWY]: 1000 ms (1 sekunda) - aby symulacja przebiegała szybko.
// [TRYB DOCELOWY / PRODUKCJA]: 60000 ms (60 sekund) - zgodnie ze schematem.
#define GREENHOUSE_READ_INTERVAL_MS   1000

// Czas trwania manualnego zraszania (uruchamianego z przycisku EXTI)
// [TRYB TESTOWY]: 10000 ms (10 sekund) - na potrzeby prezentacji.
// [TRYB DOCELOWY / PRODUKCJA]: 600000 ms (10 minut) - zgodnie ze schematem.
#define MANUAL_WATERING_DURATION_MS   (10 * 1000)

/* UWAGA O SYMULACJI: 
   Obecnie czujniki (GetTemp1, itp.) w warstwie HAL zwracają wygenerowane liczby,
   które powoli się zmieniają. Docelowo w funkcji Task_Read należy podpiąć 
   rzeczywisty odczyt z ADC/I2C w pliku greenhouse_hal_wrapper.c. */

// Domyślne mapowanie pinów przycisku i diody (w przypadku braku generacji w main.h)
#ifndef BUTTON_Pin
#define BUTTON_Pin                    GPIO_PIN_0
#endif

#ifndef LD3_Pin
#define LD3_Pin                       GPIO_PIN_3
#define LD3_GPIO_Port                 GPIOB
#endif

/* ========================================================================== */
/* --- SEKCJA 2: STRUKTURY DANYCH ---                                        */
/* ========================================================================== */

// Struktura przechowująca pomiary z czujników w formacie stałoprzecinkowym.
// Wartości pomnożone przez 10 (np. 245 = 24.5C, 552 = 55.2%).
// Pozwala to na rezygnację z typu float na małym rdzeniu Cortex-M0.
typedef struct {
    int16_t temp1;       // Temperatura z czujnika 1 (Logika Ogrzewania)
    int16_t temp2;       // Temperatura z czujnika 2 (Używana do obliczania średniej)
    int16_t temp3;       // Temperatura z czujnika 3 (Logika Klap Wentylacyjnych)
    uint16_t humidity;   // Wilgotność powietrza/gleby (Logika Auto Podlewania)
} Greenhouse_Sensors_t;

/* ========================================================================== */
/* --- SEKCJA 3: PUNKTY WEJŚCIOWE ZADAŃ RTOS ORAZ TIMERÓW ---                 */
/* ========================================================================== */

// Zadanie wątku odczytu czujników. Odpytuje sensory co określony interwał.
void Task_Read_Rtn(void *argument);

// Zadanie wątku głównego (logiki sterowania). Odbiera dane z kolejki i steruje wyjściami.
void Task_Logic_Rtn(void *argument);

// Callback wywoływany przez timer one-shot po zakończeniu podlewania manualnego.
void ManualWateringTimer_Callback(void *argument);

// Okresowy callback (100ms) realizujący miganie diody statusowej (LD3).
void LEDTimer_Callback(void *argument);

/* ========================================================================== */
/* --- SEKCJA 4: GETTERY STANU AKTUATORÓW ---                                 */
/* ========================================================================== */

uint8_t Greenhouse_Logic_GetHeaterState(void);
uint8_t Greenhouse_Logic_GetFlapsState(void);
uint8_t Greenhouse_Logic_GetWateringState(void);

/* ========================================================================== */
/* --- SEKCJA 5: ZASOBY SYSTEMOWE (UCHWYTY FREERTOS) ---                     */
/* ========================================================================== */

extern osMessageQueueId_t greenhouseQueueHandle;
extern osTimerId_t manualWateringTimerHandle;
extern osTimerId_t ledTimerHandle;
extern osSemaphoreId_t buttonSemaphoreHandle;

#ifdef __cplusplus
}
#endif

#endif /* GREENHOUSE_LOGIC_H */
