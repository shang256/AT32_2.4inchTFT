# CODEBUDDY.md This file provides guidance to CodeBuddy when working with code in this repository.

## Overview

Embedded firmware demo for the **ArteryTek AT32F403ACGU7** (ARM Cortex-M4F @ 240MHz) driving a **ST7789V 240x240 LCD** via hardware SPI1, running **FreeRTOS v202212.01 + LVGL v8.3**. A rotary encoder adjusts PWM backlight brightness and toggles a button state in the LVGL UI; an SGP30 air-quality sensor (eCO2/TVOC) reports over a software I2C bus. The project is a **Keil MDK5 / uVision** project (ARM Compiler 6, i.e. AC6).

## Commands

### Build

Open `Project/AT32F403A_LCD_Demo.uvprojx` in Keil MDK5 (?Vision) and press `F7` (or Build). This requires the `ArteryTek.AT32F403A_407_DFP` device support pack to be installed. The output is `Project/Objects/AT32F403A_LCD_Demo.hex`; a post-build step runs `fromelf.exe --bin` to also produce a `.bin` in `Project/Output/`. There is no Makefile/CMake build path ¨• Keil is the canonical build system.

### Flash / Debug

Flash with an **SWD** debugger (a J-Link config `JLinkSettings.ini` is present). JTAG must NOT be used: `encoder_init()` repurposes PB3/PB4 (JTDO/NJTRST) as GPIO, switching to SWD-only via `SWJTAG_MUX_010`.

### Clean

Run `keilkilll.bat` (Windows batch) to delete intermediate build artifacts (`*.o`, `*.crf`, `*.axf`, `*.lst`, `*.map`, etc.).

### Lint / IDE IntelliSense

There is no dedicated linter or unit-test harness. `compile_flags.txt` supplies absolute include paths (pointing at this workspace) and defines for clangd/LSP editor diagnostics.

## Architecture

### Layering

The code follows a strict one-way dependency order: `Application ¨L Middleware ¨L Drivers ¨L Libraries`.

- **`Application/`** ¨• business logic and task orchestration. `app_main.c/h` owns `main()`, clock init, and FreeRTOS task/queue creation; `app_ui.c/h` owns the LVGL demo UI and consumes events.
- **`Middleware/`** ¨• OS/graphics configuration and ports. `freertos/FreeRTOSConfig.h` (240MHz / 1kHz tick / 20KB heap), `lvgl/lv_conf.h`, `lvgl/lv_port_disp.c/h` (draw buffer + flush callback), `lvgl/lv_rtos_tick.h` (LVGL heartbeat bridge).
- **`Drivers/`** ¨• peripheral drivers, one pair per peripheral, independent of each other: `lcd_st7789`, `backlight` (TMR3 PWM), `encoder` + `encoder_accel`, `lowpower`, `sw_i2c`, `sgp30`, `delay`, and `at32f403a_407_it` (interrupt dispatch).
- **`Libraries/`** ¨• third-party/vendor code: ArteryTek CMSIS + standard-peripheral library, FreeRTOS kernel, and LVGL source with demos.

Rules enforced by this layering: drivers never call each other; `Application` never touches registers directly; LVGL APIs are only invoked from the LVGL task.

### FreeRTOS task model (thread-safety backbone)

| Task | Priority | Stack | Role |
|------|----------|-------|------|
| `encoder_task` | 3 | 1KB | 20ms poll of the encoder driver; posts `app_encoder_msg_t` to a queue; handles 3s long-press shutdown sequence. |
| `lvgl_task` | 2 | 4KB | Sole owner of LVGL: 5ms `lv_timer_handler()`; drains encoder/sensor queues to drive the UI. |
| `sgp30_task` | 1 | 1KB | 1s-period SGP30 read; posts `app_sensor_msg_t` to a queue. |
| idle | 0 | 512B | FreeRTOS built-in idle task. |

Because LVGL is not thread-safe, **all** UI access is confined to `lvgl_task`. Cross-thread data flows only through FreeRTOS queues (`g_encoder_queue`, `g_sensor_queue`), whose item structs use uniform `int32_t`/`uint32_t` fields to avoid mixed-width padding. Producer tasks interact only with drivers and queues, never LVGL.

### Power management (the most subtle area)

The board boots into a **DEEPSLEEP (stop mode) "off" state**. `main()` early on calls `lowpower_boot_wait()`, which arms PB3 (`EXINT3` falling edge) and stops; pressing the encoder push wakes the device (resumes from `WFI`, not reset) and re-enables the 240MHz PLL. A 3-second long-press runs a shutdown sequence (draw "Power off...", `backlight_off()`, wait for release, enter stop). A wakeup is only accepted if PB3 is held low 50ms (debounce against contact-bounce "fake" falling edges); there is a 200ms quiet period before entering stop, and pending TMR7/SysTick/EXINT3 flags are cleared before `WFI`.

Critical constraint: after wakeup, the clock is restored **without calling `crm_reset()`** ¨• during stop the CRM clock gates are preserved, and resetting them would kill TMR7/SPI1 etc. `Drivers/lowpower.c` manipulates PWC register bit-fields directly (the stock `at32f403a_407_pwc.c` is intentionally not included). A full STANDBY alternative is also implemented in `lowpower.c` for future use.

### LVGL / FreeRTOS integration specifics

- SysTick is taken over: `SysTick_Handler` calls `xPortSysTickHandler`; PendSV/SVC are redirected via `FreeRTOSConfig.h` macros.
- `LV_TICK_CUSTOM=1` reads `xTaskGetTickCount()` directly (1ms tick), so no `lv_tick_inc()` call sites exist; include-order bridging lives in `lv_rtos_tick.h`.
- Memory: FreeRTOS heap_4 = 20KB (stacks/queues), LVGL TLSF = 32KB (UI objects), draw buffer = 240x40 (~18.8KB). Total RAM ~77KB/96KB; Flash ~160KB/1MB.

### Key driver interfaces

- `backlight_init()` / `backlight_set_percent()` ¨• TMR3_CH4 PWM on PB1, clamped 20Ã¿100%.
- `encoder_init()` / `encoder_get_delta()` / `encoder_push_pressed()` / `encoder_push_rising_edge()` ¨• TMR7 1ms synchronous sampling + 4x state-table decode (jitter-immune); also maintains long-press duration.
- `encoder_accel_init()` / `encoder_accel_feed()` ¨• maps raw delta to "steps" with slow-precise / fast-accelerating response; a reusable `encoder_accel_t` instance per use-case.
- `sgp30_init()` / `sgp30_read()` ¨• software I2C (PA2/PA3, open-drain), `Init_air_quality` (0x2003) + `Measure` (0x2008) with CRC.
- `LCD_Init()` / `LCD_WriteAreaBytes()` ¨• SPI1 init + window RGB565 writes used by the LVGL flush callback.

### Hardware pin map (quick reference)

PB1 = backlight PWM; PB0/PA4/PA6 = LCD CS/DC/RES; PA5/PA7 = SPI1 SCK/MOSI; PB3 = encoder push (EXINT3, wake) / PB4-PB5 = encoder A/B (external pull-up); PA2/PA3 = SGP30 software I2C. PB3/PB4 are JTAG pins released to GPIO at `encoder_init` (SWD-only debug).
