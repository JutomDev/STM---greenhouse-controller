# Migration Guide: Porting Greenhouse Firmware to High-End STM32 Targets (Cortex-M7 / STM32CubeIDE)

## 1. Overview
This technical guide details the migration procedure for porting the greenhouse climate controller from the ultra-low-power **STM32F031K6** (ARM Cortex-M0, 32 KB Flash, 4 KB SRAM) to high-performance microcontrollers such as the **STM32F7 series** (ARM Cortex-M7, e.g., Nucleo-F767ZI / Nucleo-F746ZG with 1+ MB Flash, 320+ KB SRAM) or similar Cortex-M4/M33 devices.

---

## 2. Hardware and Pinout Re-mapping

When migrating across hardware form factors (e.g., from 32-pin Nucleo-32 to 144-pin Nucleo-144), board net assignments must be re-mapped:

### A. GPIO Indicators and Buttons
| Function | Original (NUCLEO-F031K6) | Target Example (NUCLEO-F767ZI) | Action Required |
| :--- | :--- | :--- | :--- |
| **Manual Trigger Button** | `PA0` (EXTI Line 0) | `PC13` (User Button B1, EXTI Line 13) | Update `BUTTON_Pin` and `BUTTON_GPIO_Port` definitions |
| **Status Beacon (LD3)** | `PB3` (Green LD3) | `PB0` (Green LD1) / `PB7` (Blue LD2) / `PB14` (Red LD3) | Update `LD3_Pin` and `LD3_GPIO_Port` definitions |

Update `Core/Inc/greenhouse_logic.h` (or rely on CubeMX generated definitions in `main.h`):
```c
#define BUTTON_Pin            GPIO_PIN_13
#define BUTTON_GPIO_Port      GPIOC

#define LD3_Pin               GPIO_PIN_0
#define LD3_GPIO_Port         GPIOB
```

### B. UART Virtual COM Port (VCP)
* Original STM32F031 target routes diagnostic console output via **USART1** (`PA2`/`PA15`).
* Nucleo-144 boards route the ST-LINK Virtual COM Port to **USART3** (`PD8` TX, `PD9` RX) or **USART2** (`PD5` TX, `PD6` RX).
* Required code modification in `Core/Src/greenhouse_hal_wrapper.c`:
  1. Change peripheral handle declaration:
     ```c
     extern UART_HandleTypeDef huart3; /* replace huart1 */
     ```
  2. Direct transmission calls (`HAL_UART_Transmit`) to `&huart3`.

---

## 3. STM32CubeMX Project Initialization

The recommended porting workflow is generating a clean CubeMX project targeting the new MCU, then transplanting the decoupled application layers:

1. **Create New Project**: Select target MCU/board (e.g., `NUCLEO-F767ZI`).
2. **Configure Middleware (FreeRTOS)**:
   - Enable **FREERTOS** under Middleware with **CMSIS_V2** API.
   - Replicate the exact RTOS primitive definitions:
     - Thread: `readTask` (Entry point: `Task_Read_Rtn`, Priority: `osPriorityNormal`)
     - Thread: `defaultTask` (Entry point: `Task_Logic_Rtn`, Priority: `osPriorityNormal`)
     - Message Queue: `greenhouseQueue` (Queue size: 2, Element size: `sizeof(Greenhouse_Sensors_t)`)
     - Binary Semaphore: `buttonSemaphore`
     - Mutex: `uartMutex`
     - Software Timer (One-shot): `manualWateringTimer` (Callback: `ManualWateringTimer_Callback`)
     - Software Timer (Periodic 100ms): `ledTimer` (Callback: `LEDTimer_Callback`)
3. **Configure Peripherals**:
   - Enable console UART (e.g., USART3, 115200 or 38400 baud, 8N1).
   - Configure button pin as `GPIO_EXTI` (falling-edge trigger) and enable corresponding EXTI interrupt line in NVIC.
   - Configure LED pin as `GPIO_Output` push-pull.
4. **Clock Tree**:
   - Set core frequency to maximum rating (e.g., 216 MHz on STM32F7) via PLL multiplier.
5. **Timebase Source**:
   - Under `System Core -> SYS`, set **Timebase Source** to a hardware timer (`TIM1` or `TIM6`) instead of `SysTick`. FreeRTOS manages `SysTick`; using a dedicated hardware timer prevents HAL driver timeout deadlocks.

---

## 4. Application Integration Checklist

1. Copy application files into the new project:
   - `Core/Src/greenhouse_logic.c`
   - `Core/Src/greenhouse_hal_wrapper.c`
   - `Core/Inc/greenhouse_logic.h`
   - `Core/Inc/greenhouse_hal_wrapper.h`
2. Include the header in `Core/Src/main.c`:
   ```c
   #include "greenhouse_logic.h"
   ```
3. In `Core/Src/freertos.c`, route the generated task entry points to the application functions:
   ```c
   void Task_Read_Rtn(void *argument);
   void Task_Logic_Rtn(void *argument);
   ```
4. Verify HAL header compatibility: Replace `#include "stm32f0xx_hal.h"` with `#include "stm32f7xx_hal.h"` (or rely on `main.h` inclusion).

---

## 5. Critical Architectural Differences (Cortex-M0 vs Cortex-M7)

### A. NVIC Interrupt Priority Grouping
Cortex-M0 utilizes a fixed 2-bit NVIC without subpriority grouping. Cortex-M7 implements a 4-bit NVIC with preempt/subpriority fields:
* **Failure Mode**: Calling FreeRTOS API functions (e.g., `osSemaphoreRelease`) from an ISR whose priority is numerically lower (higher urgency) than `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` triggers a kernel panic or immediate `HardFault`.
* **Remediation**:
  1. Enforce standard priority grouping in initialization:
     ```c
     HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
     ```
  2. Set the button EXTI interrupt priority to a value **greater than or equal to** `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (typically priority 5 through 15).

### B. Task Stack Sizing
* On STM32F031, stack depths were constrained (48 words for `readTask`, 100 words for `defaultTask`) to survive in 4 KB RAM.
* On Cortex-M7, hardware exception stacking includes additional registers (plus lazy FPU context frames of 34 words when floating-point operations occur). Stacks sized under 128 words risk stack overflow.
* **Remediation**: Increase task stack sizes in CubeMX to a minimum of **256 words** (1024 bytes) or **512 words** (2048 bytes).

### C. Floating-Point Unit (FPU) & String Formatting
* The original firmware avoids `snprintf` and `float` types entirely, utilizing fixed-point integer conversions (`append_fixed`, `u32_to_str`) to save ~3.5 KB Flash and prevent stack inflation.
* On Cortex-M7, a single/double precision hardware FPU is available. Once Hardware FP is enabled in compiler toolchain flags (`-mfpu=fpv5-sp-d16 -mfloat-abi=hard`), standard IEEE-754 single-precision arithmetic executes in single-cycle instructions without memory penalties.

### D. Data Cache (D-Cache) and DMA Coherency
Cortex-M7 cores feature separate L1 instruction and data caches (I-Cache and D-Cache):
* **Failure Mode**: If UART or ADC transfers are migrated to DMA, the DMA controller accesses physical SRAM directly, bypassing the processor's D-Cache. If the CPU writes a transmission buffer, stale data remains in cache and corrupted packets are transmitted.
* **Remediation**:
  1. Invalidate or clean cache lines before DMA operations:
     ```c
     SCB_CleanDCache_by_Addr((uint32_t *)tx_buffer, buffer_len);
     ```
  2. Alternatively, place DMA buffers in a non-cacheable SRAM section using MPU (Memory Protection Unit) region attributes.
