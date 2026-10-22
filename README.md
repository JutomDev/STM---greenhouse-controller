# Greenhouse Climate Controller (FreeRTOS)

## 1. Overview
This project is a deterministic greenhouse microclimate control system developed for the resource-constrained STM32F031K6 microcontroller (ARM Cortex-M0, 32 KB Flash, 4 KB SRAM) running FreeRTOS under the CMSIS-RTOS V2 interface. It acquires sensor telemetry (three temperature channels and soil/air humidity) and automatically manages heating, ventilation flaps, and irrigation under tight hardware limitations. The firmware avoids dynamic memory allocation, floating-point units, and standard C library string formatting routines to ensure predictable, zero-fragmentation execution within 4 KB RAM.

---

## 2. Architecture and Hardware

### Hardware Mapping & Bus Pinout
| Pin | Function / Net | Peripheral / Mode | Direction | Electrical / Driver Notes |
| :--- | :--- | :--- | :--- | :--- |
| `PA0` | Manual Watering Trigger | `EXTI0` (Interrupt) | Input | Internal Pull-Up, falling-edge trigger; debounced via binary semaphore |
| `PA2` | Diagnostic Telemetry TX | `USART1_TX` (AF1) | Output | Push-Pull, 38400 baud, 8N1, connected to ST-LINK VCP |
| `PA15` | Telemetry RX (Reserved) | `USART1_RX` (AF1) | Input | Floating / ST-LINK VCP bridge |
| `PB3` | Status Beacon (LD3) | `GPIO_Output` | Output | Push-Pull, driven by software timer callback / bare-metal HardFault loop |

### Clock Configuration
* Core Clock (`SYSCLK`): 48 MHz (Internal HSI 8 MHz with PLL x12 multiplier).
* System Busses (`AHB`, `APB`): 48 MHz (Prescalers set to 1).
* HAL Timebase: Hardware timer `TIM1` generates the 1 kHz HAL tick to avoid contention with the FreeRTOS `SysTick` handler.

### FreeRTOS Static Memory Allocation
Dynamic heap allocation (`pvPortMalloc`) is bypassed (`configTOTAL_HEAP_SIZE` = 64 bytes for CMSIS-RTOS V2 initialization compatibility). All task control blocks, stacks, queues, mutexes, and timers are allocated in the static `.bss` section:

| Object Name | RTOS Primitive | Stack (Words) | Stack (Bytes) | TCB / Header | Total RAM | Role |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `readTask` | Thread | 48 words | 192 B | 84 B (`StaticTask_t`) | 276 B | Periodically samples telemetry and pushes to queue |
| `defaultTask` | Thread | 100 words | 400 B | 84 B (`StaticTask_t`) | 484 B | Core logic engine: processes data, manages actuators |
| `Tmr Svc` | Kernel Thread | 128 words | 512 B | 84 B (`StaticTask_t`) | 596 B | FreeRTOS timer daemon task |
| `Idle Task` | Kernel Thread | 80 words | 320 B | 84 B (`StaticTask_t`) | 404 B | System idle loop and power management |
| `greenhouseQueue` | Message Queue | -- | 16 B (2x8 B) | 76 B (`StaticQueue_t`) | 92 B | Ring buffer holding sensor payloads (`Greenhouse_Sensors_t`) |
| `buttonSemaphore` | Binary Semaphore | -- | -- | 76 B (`StaticSemaphore_t`) | 76 B | Deferred interrupt signaling from EXTI0 ISR |
| `uartMutex` | Mutex | -- | -- | 76 B (`StaticSemaphore_t`) | 76 B | Re-entrant lock protecting non-blocking USART1 transfers |
| `manualWateringTimer` | Software Timer | -- | -- | 36 B (`StaticTimer_t`) | 36 B | One-shot timeout counter for manual watering cutoff |
| `ledTimer` | Software Timer | -- | -- | 36 B (`StaticTimer_t`) | 36 B | Periodic 100 ms cadence generator for status LED patterns |

Memory Footprint Summary (Debug Build, GCC 15.2.1):
* Flash: 30580 B / 32768 B (93.32%)
* SRAM: 3480 B / 4096 B (84.96%)
* Unallocated SRAM: 616 B reserved for Main Stack Pointer (MSP) and ISR execution contexts.

---

## 3. System Block Diagram

```mermaid
graph TD
    subgraph Hardware_And_Interrupts [Hardware & Interrupt Layer]
        Sensors["Sensors / Simulator"]
        Button["Button Pin PA0 (EXTI0)"]
        LED["Status LED PB3 (LD3)"]
        Actuators["Relay Drivers (Heater / Flaps / Sprinklers)"]
        UART["USART1 Peripheral (PA2/PA15)"]
    end

    subgraph RTOS_Kernel [FreeRTOS CMSIS-RTOS V2 Kernel]
        ReadTask["Task_Read_Rtn (Priority: Normal)"]
        LogicTask["Task_Logic_Rtn (Priority: Normal)"]
        Queue["greenhouseQueue (Capacity: 2 items)"]
        Sem["buttonSemaphore (Binary)"]
        Mutex["uartMutex (Recursive)"]
        WaterTimer["manualWateringTimer (One-shot)"]
        LEDTimer["ledTimer (Periodic 100ms)"]
    end

    Sensors -->|Telemetry Sample| ReadTask
    ReadTask -->|osMessageQueuePut (Timeout: 100ms)| Queue
    Button -->|Falling Edge IRQ| EXTI_ISR["HAL_GPIO_EXTI_Callback()"]
    EXTI_ISR -->|osSemaphoreRelease| Sem

    Queue -->|osMessageQueueGet (Timeout: 100ms)| LogicTask
    Sem -->|osSemaphoreAcquire (Non-blocking: 0ms)| LogicTask

    LogicTask -->|Start / Reset| WaterTimer
    WaterTimer -->|Timeout Callback| WaterCB["ManualWateringTimer_Callback()"]
    WaterCB -->|Clear Manual Flag| LogicTask

    LogicTask -->|Update Actuator State Flags| LEDTimer
    LEDTimer -->|100ms Tick Callback| LEDCB["LEDTimer_Callback()"]

    LogicTask -->|Discrete Commands| Actuators
    LEDCB -->|Blink Pattern Output| LED
    ReadTask -.->|Protected Print| Mutex
    LogicTask -.->|Protected Print| Mutex
    Mutex -->|HAL_UART_Transmit| UART
```

---

## 4. Quickstart and CLI Build

### Prerequisites
* Toolchain: `arm-none-eabi-gcc` (tested with GCC 15.2.1 / 13.x)
* Build System: `cmake` (version >= 3.22) and `ninja`
* Hardware Programmer: `openocd` (version >= 0.12.0) with ST-LINK interface

### 1. Configure Build Tree
```bash
cmake -B build/Debug -S . -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

### 2. Compile Firmware
```bash
cmake --build build/Debug
```

### 3. Flash Target via OpenOCD
Connect the NUCLEO-F031K6 via USB and execute:
```bash
openocd -f board/st_nucleo_f0.cfg \
  -c "program build/Debug/STM_freertos_szklarania.elf verify reset exit"
```

### 4. Serial Telemetry Monitor
Open a serial terminal connected to the ST-LINK Virtual COM Port:
* Baud Rate: `38400`
* Data Bits: `8`
* Parity: `None`
* Stop Bits: `1`
* Flow Control: `None`

Expected Initial Output:
```text
--- BOOT CHECK ---
[OK] uartMutex is created
[OK] buttonSemaphore is created
[OK] manualWateringTimer is created
[OK] ledTimer is created
[OK] greenhouseQueue is created
[OK] defaultTask is created
[OK] readTask is created
--- END BOOT CHECK ---
DBG: readTask started
S:1 T1:10.0 T2:10.0 T3:25.0 H:55.0 B:0
DBG: defaultTask started
T:10.0,10.0,25.0 H:55.0
```

---

## 5. Configuration and Operating Logic

### Fixed-Point Integer Arithmetic
To eliminate the flash and RAM overhead of `printf`/`snprintf` floating-point formatting, telemetry values are stored as fixed-point integers scaled by a factor of 10 (`int16_t` and `uint16_t`, 1 LSB = 0.1):
* Raw value `235` represents `23.5 deg C`.
* Raw value `600` represents `60.0 % RH`.
Telemetry formatting is handled via deterministic integer-to-ASCII utility functions (`u32_to_str`, `fixed_to_str`, `append_fixed`) operating on static buffers.

### Control Rules and Thresholds
| Actuator | Control Rule | Threshold Value | Persistence Filter | Action |
| :--- | :--- | :--- | :--- | :--- |
| **Heater** | Emergency Low Temperature | `T1 <= 1.0 deg C` OR `T_avg <= 5.0 deg C` | 0 cycles (Immediate) | Heater `ON` |
| **Heater** | Sustained Cold Condition | `T1 < 4.0 deg C` | 5 consecutive sample cycles | Heater `ON` |
| **Heater** | High Temperature Cutoff | `T1 >= 6.0 deg C` | 0 cycles (Immediate) | Heater `OFF` |
| **Flaps** | High Internal Temperature | `T3 > 28.0 deg C` | 10 consecutive sample cycles | Flaps `OPEN` |
| **Flaps** | Low Temperature Cutoff | `T3 <= 24.0 deg C` | 0 cycles (Immediate) | Flaps `CLOSE` |
| **Sprinklers** | Soil Moisture Deficit | `Humidity < 40.0 %` | 0 cycles (Immediate) | Sprinklers `ON` |
| **Sprinklers** | Moisture Saturation Cutoff | `Humidity >= 60.0 %` | 0 cycles (Immediate) | Sprinklers `OFF` |
| **Sprinklers** | Manual Override | `PA0` Trigger (EXTI0) | One-shot timer (10s test / 600s prod) | Sprinklers `ON` |

### LED Diagnostic Indicator Codes (`PB3` / `LD3`)
The periodic 100 ms software timer modulates the green LED based on active system priority:
* `[+]` **Solid ON**: Irrigation active (automatic or manual override, highest visual priority).
* `[+]` **Fast Blink (5 Hz)**: Heating active (100 ms ON / 100 ms OFF).
* `[+]` **Slow Blink (1 Hz)**: Ventilation flaps open (500 ms ON / 500 ms OFF).
* `[+]` **Heartbeat Flash**: Standby / Idle (100 ms pulse every 2000 ms).
* `[x]` **Critical Fault (10 Hz)**: `HardFault_Handler` bare-metal spin-loop directly toggles GPIO BSRR/BRR registers with interrupts disabled.

### Tuning Macro Definitions
Key operational parameters can be adjusted directly in header and source definitions:
* Sensor sample period: `GREENHOUSE_READ_INTERVAL_MS` in `Core/Inc/greenhouse_logic.h` (default: `1000` ms for testing; configure to `60000` ms for production).
* Manual watering duration: `MANUAL_WATERING_DURATION_MS` in `Core/Src/greenhouse_logic.c` (default: `10000` ms for testing; configure to `600000` ms for production).
* UART baud rate: `huart1.Init.BaudRate` in `Core/Src/usart.c` (configured at `38400` baud).

---

## 6. Target Porting & Migration Guide
For detailed instructions on porting this firmware from the resource-constrained Cortex-M0 (STM32F031) architecture to high-performance Cortex-M7 / Cortex-M4 platforms (e.g. Nucleo-F767ZI) under STM32CubeIDE, consult the technical migration manual:
* [docs/MIGRATION_GUIDE.md](docs/MIGRATION_GUIDE.md) — Covers NVIC priority grouping, FPU stack sizing, and cache coherency rules.
