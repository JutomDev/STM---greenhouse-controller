# Przewodnik migracji projektu na inny mikrokontroler STM32 (np. Nucleo-F7) w środowisku STM32CubeIDE

Ten dokument opisuje krok po kroku, jak przenieść projekt kontrolera szklarni (`STM_freertos_szklarania`), oryginalnie napisany dla małego mikrokontrolera **STM32F031X6** (Cortex-M0, 4KB RAM, 32KB FLASH), na mocniejszy układ, np. **Nucleo-F7** (seria STM32F7, rdzeń Cortex-M7, 320KB+ RAM, 1MB+ FLASH).

---

## 1. Konfiguracja sprzętowa (Hardware Re-mapping)

Przenosząc projekt na inną płytkę (np. Nucleo-F767ZI / F746ZG), musisz zmapować fizyczne połączenia na nowe piny.

### A. Diody LED (LD3) oraz przycisk (Button)
W pliku [greenhouse_logic.h](file:///C:/Users/Julek/Documents/STM_zajecia/Szklarnia/STM_freertos_szklarania/Core/Inc/greenhouse_logic.h) zdefiniowane są piny wejścia/wyjścia:
*   **Przycisk manualnego podlewania (`BUTTON_Pin`):** Domyślnie zdefiniowany na `GPIO_PIN_0`. Na płytkach Nucleo niebieski przycisk użytkownika (User Button) jest podłączony do **PC13** (`GPIO_PIN_13`, `GPIOC`).
*   **Statusowa dioda LED (`LD3_Pin`):** Domyślnie `GPIO_PIN_3` na porcie `GPIOB`. Na płytkach Nucleo-F7 zielona dioda LED (LD1) to **PB0**, niebieska (LD2) to **PB7**, a czerwona (LD3) to **PB14**.

**Co należy zmienić w kodzie:**
W pliku `greenhouse_logic.h` (linie 30-37) zaktualizuj definicje pinów lub usuń te bloki warunkowe `#ifndef`, jeśli konfigurujesz je w CubeMX (wtedy wygenerują się w `main.h`):
```c
#define BUTTON_Pin            GPIO_PIN_13
#define BUTTON_GPIO_Port      GPIOC

#define LD3_Pin               GPIO_PIN_14
#define LD3_GPIO_Port         GPIOB
```

### B. Interfejs UART / Console Log
*   Oryginalny projekt używa **USART1** (`huart1`) na pinach PA9/PA10.
*   Płytki Nucleo przekierowują wbudowany programator (ST-LINK Virtual COM Port - VCP) na inny port, najczęściej **USART3** (piny PD8/PD9) lub **USART2** (piny PD5/PD6 lub PA2/PA3).
*   **Co należy zmienić:** W pliku [greenhouse_hal_wrapper.c](file:///C:/Users/Julek/Documents/STM_zajecia/Szklarnia/STM_freertos_szklarania/Core/Src/greenhouse_hal_wrapper.c) (linia 16 i 146) uchwyt USART jest zadeklarowany jako `huart1`. Jeśli nowy mikrokontroler używa np. USART3, musisz:
    1.  Zmienić `extern UART_HandleTypeDef huart1;` na `extern UART_HandleTypeDef huart3;`.
    2.  W funkcjach transmisji (`HAL_UART_Transmit`) zmienić referencję z `&huart1` na `&huart3`.

---

## 2. Przeniesienie konfiguracji w CubeMX (plik .ioc)

Najbezpieczniejszą metodą migracji jest wygenerowanie nowego projektu dla nowego kontrolera w STM32CubeIDE i skopiowanie plików logiki biznesowej.

### Krok po kroku w CubeMX / STM32CubeIDE:
1.  **Stwórz nowy projekt** (File -> New -> STM32 Project) i wybierz swój docelowy mikrokontroler (np. `STM32F767ZI` lub płytkę `NUCLEO-F767ZI`).
2.  **Skonfiguruj system operacyjny (FreeRTOS):**
    *   W zakładce *Middleware* włącz **FREERTOS** (zalecany interfejs API **CMSIS_V2**).
    *   Stwórz zadania (Tasks), kolejki (Queues), semafory (Semaphores) oraz timery (Timers) o dokładnie takich samych nazwach jak w starym projekcie:
        *   Zadanie: `readTask` (funkcja wejściowa: `Task_Read_Rtn`)
        *   Zadanie: `defaultTask` (funkcja wejściowa: `Task_Logic_Rtn`)
        *   Kolejka wiadomości: `greenhouseQueueHandle`
        *   Semafor binarny: `buttonSemaphoreHandle`
        *   Timer programowy (Single Shot): `manualWateringTimerHandle`
        *   Timer programowy (Periodic): `ledTimerHandle`
        *   Mutex: `uartMutexHandle`
3.  **Skonfiguruj peryferia:**
    *   Włącz odpowiedni port USART (np. USART3) w trybie Asynchronous (115200 baud, 8N1).
    *   Skonfiguruj pin diody jako GPIO_Output.
    *   Skonfiguruj pin przycisku jako **GPIO_EXTI** (External Interrupt) z wyzwalaniem na zbocze opadające/rosnące (Falling/Rising edge) i włącz odpowiednie przerwanie EXTI w kontrolerze NVIC.
4.  **Skonfiguruj zegar systemowy (SystemClock_Config):**
    *   Dla mikrokontrolera STM32F7 możesz ustawić częstotliwość taktowania rdzenia aż do 216 MHz. Użyj kreatora zegarów w CubeMX, aby skonfigurować PLL na maksimum.
5.  **Wygeneruj kod:** Kliknij *Alt+K* lub ikonę generowania kodu.

---

## 3. Przeniesienie i integracja kodu aplikacji

Po wygenerowaniu szkieletu projektu dla nowego mikrokontrolera:
1.  Skopiuj pliki logiki biznesowej do nowego projektu:
    *   `greenhouse_logic.c` i `greenhouse_hal_wrapper.c` do folderu `/Core/Src/`
    *   `greenhouse_logic.h` i `greenhouse_hal_wrapper.h` do folderu `/Core/Inc/`
2.  W pliku `main.c` nowego projektu dodaj include:
    ```c
    #include "greenhouse_logic.h"
    ```
3.  W pliku `freertos.c` nowego projektu upewnij się, że wygenerowane zadania przekazują sterowanie do Twoich funkcji, np.:
    ```c
    void Task_Read_Rtn(void *argument);
    void Task_Logic_Rtn(void *argument);
    ```
4.  Podmień nagłówek biblioteki HAL w plikach `.h`:
    *   Zamiast `#include "stm32f0xx_hal.h"` w pliku [main.h](file:///C:/Users/Julek/Documents/STM_zajecia/Szklarnia/STM_freertos_szklarania/Core/Inc/main.h) i innych plikach, CubeMX automatycznie wygeneruje odpowiedni nagłówek dla nowej serii, np. `#include "stm32f7xx_hal.h"`. Upewnij się, że Twoje własne pliki nagłówkowe referują do właściwego `main.h`.

---

## 4. Krytyczne różnice architektoniczne (Na co zwrócić uwagę)

Migracja z Cortex-M0 (STM32F0) na Cortex-M7 (STM32F7) przynosi ogromne zmiany wydajnościowe, ale wymaga uwagi w kilku miejscach:

### A. Priorytety Przerwań w NVIC (Bardzo Ważne!)
Rdzenie Cortex-M0 nie posiadają zaawansowanego grupowania priorytetów przerwań. Rdzenie Cortex-M7 (i M4/M3) mają 4-bitowy kontroler NVIC z obsługą podgrup priorytetów.
*   **Problem:** Jeśli Twoje przerwanie EXTI (od przycisku) wywołuje API FreeRTOS (np. `osSemaphoreRelease`), jego priorytet **musi** być poprawnie skonfigurowany w NVIC.
*   **Rozwiązanie:** 
    1.  W nowym projekcie upewnij się, że ustawiono: `HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4)`.
    2.  Priorytet przerwania EXTI (w CubeMX w zakładce *System Core -> NVIC*) musi być ustawiony na wartość numeryczną **równą lub większą** niż `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (domyślnie w FreeRTOS jest to zazwyczaj 5). Ustawienie priorytetu np. na 0-4 spowoduje natychmiastowy crash systemu (HardFault) przy naciśnięciu przycisku.

### B. Rozmiary Stosów Zadań (Task Stack Sizes)
*   **STM32F031** ma tylko 4KB RAM, przez co stosy zadań w tym projekcie były skrajnie małe (np. 64 lub 128 słów).
*   **STM32F7** ma setki kilobajtów pamięci RAM. Stosy zadań rzędu 128 słów mogą okazać się zbyt małe na architekturze Cortex-M7 z powodu większej ramki stosu (np. przy obsłudze rejestrów FPU).
*   **Zalecenie:** W nowym projekcie w CubeMX zwiększ rozmiary stosu (Stack Size) zadań `readTask` oraz `defaultTask` do minimum **256 słów** (1024 bajty) lub **512 słów** (2048 bajtów).

### C. Jednostka Zmiennoprzecinkowa (FPU) i Formatowanie Tekstu
*   Oryginalny kod celowo rezygnuje z funkcji `snprintf` i liczb zmiennoprzecinkowych (`float`/`double`), aby oszczędzić FLASH i RAM na Cortex-M0.
*   **Cortex-M7** posiada sprzętowy koprocesor FPU (Single/Double Precision). 
*   Jeśli przenosisz projekt na układ F7, ograniczenia te znikają. Możesz włączyć obsługę FPU w opcjach projektu w STM32CubeIDE (Project Properties -> C/C++ Build -> Settings -> Tool Settings -> Floating point hardware -> Hardware FP). 
*   Po włączeniu FPU możesz powrócić do używania standardowego `snprintf` i typu `float` do odczytu czujników, bez obawy o przepełnienie stosu czy spowolnienie działania programu.

### D. Pamięć Podręczna Cache (D-Cache i I-Cache)
Układy STM32F7 posiadają pamięć podręczną instrukcji i danych. 
*   Jeśli w nowym projekcie zdecydujesz się na optymalizację transmisji UART przy użyciu DMA (Direct Memory Access), pamięć Cache może powodować niespójność danych (DMA wyśle stare dane z pamięci RAM, zanim zostaną one zapisane z Cache'a).
*   **Rozwiązanie:** Przy użyciu DMA pamiętaj o czyszczeniu linii Cache przed transmisją (`SCB_CleanDCache_by_Addr`) lub umieść bufor transmisyjny w sekcji pamięci niebuforowanej (Non-cacheable SRAM).

### E. Timebase Source w CubeMX
*   W nowym projekcie w CubeMX, w zakładce *System Core -> SYS*, zmień **Timebase Source** z domyślnego *SysTick* na dowolny wolny timer sprzętowy (np. **TIM1** lub **TIM6**). FreeRTOS przejmie kontrolę nad *SysTick*, a HAL potrzebuje osobnego timera do poprawnego działania funkcji `HAL_Delay()` i obsługi timeoutów.
