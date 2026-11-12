# CODEBUDDY.md This file provides guidance to CodeBuddy when working with code in this repository.

## Overview

Embedded firmware demo for the **ArteryTek AT32F403ACGU7** (ARM Cortex-M4F @ 240MHz) driving a **ST7789V 240x240 LCD** via hardware SPI1, running **FreeRTOS v202212.01 + LVGL v8.3**. A rotary encoder adjusts PWM backlight brightness and toggles a button state in the LVGL UI; three sensors share one software I2C bus: SGP30 (eCO2/TVOC), AHT20 (temperature/humidity), and BMP280 (pressure + derived altitude). A full-screen 240x240 gauge image generated from a Figma design is shown as the dashboard background. The project is a **Keil MDK5 / uVision** project (ARM Compiler 6, i.e. AC6).

**Repository layout**: `Software/APP/` is the application firmware (originally at repo root, since migrated; all paths below are relative to it unless stated otherwise). `Software/Bootloader/` is a ¡Ü12KB USB-CDC bootloader that jumps to the APP and receives firmware upgrades (same layout: Application/Drivers/MDK/Project). `Software/Shared/` holds code shared by both images: `boot_shared.h` (flash partition map, parameter-page struct, upgrade protocol), `flash_drv.c/h` (register-level flash driver ¡ª the SPL `at32f403a_407_flash.c`/`at32f403a_407_usb.c` are NOT in the repo), `usb_cdc.c/h` (compact USB CDC device stack for the F1-style USBFS controller). `TOOL/upgrader/` is a Python tkinter drag-and-drop upgrade host (pyserial; optional tkinterdnd2). `TOOL/keil5_pack/keil5_pack.rar` holds the Keil device support pack.

## Commands

### Build (IDE)

Open `Software/APP/Project/AT32F403A_LCD_Demo.uvprojx` in Keil MDK5 (uVision) and press `F7`. Requires the `ArteryTek.AT32F403A_407_DFP` pack (see `TOOL/keil5_pack/`). Output: `Software/APP/Project/Objects/AT32F403A_LCD_Demo.hex`; a post-build step runs `fromelf.exe --bin` producing `Software/APP/Project/Output/AT32F403A_LCD_Demo.bin`. There is no Makefile/CMake path ¡ª Keil is canonical.

### Build (command line, verified working)

From the repo root: `C:\Keil_v5\UV4\UV4.exe -b Software\APP\Project\AT32F403A_LCD_Demo.uvprojx -j0 -o build_log.txt` (blocks until done; `-j0` = no GUI). The log lands in `Software/APP/Project/build_log.txt`. The same works for the bootloader: `... -b Software\Bootloader\Project\Bootloader.uvprojx ...` (current size ~4.7KB, budget 12KB). Exit is silent on success ¡ª always check the log for `0 Error(s)`.

### Flash / Debug

Use an **SWD** debugger (J-Link; `JLinkSettings.ini` sits in `Software/APP/Project/`). JTAG must NOT be used: `encoder_init()` repurposes PB3/PB4 (JTDO/NJTRST) as GPIO via `SWJTAG_MUX_010`. Also disable Trace in Keil debug settings ¡ª a lingering SWO probe keeps PB3 seized and breaks the encoder A/P channels.

**Bootloader + APP flashing**: flash `Software/Bootloader/Project/Objects/Bootloader.hex` to 0x08000000 and the APP hex (linked at **0x08003000**) to its base, or update the APP over TypeC with the host tool (below). BOOT enters upgrade mode when: the parameter page has `update_request` set (APP online-triggered), the APP image is invalid, or PB3 is held at power-on (rescue path).

### Upgrade host tool

`TOOL/upgrader/upgrade_host.py` (deps in `requirements.txt`, `pip install pyserial tkinterdnd2`). Run `python upgrade_host.py [firmware.bin]`; drag/drop or pick the APP `.bin`, and the tool auto-detects the device (VID 0x2E3C / PID 0x5741), commands a running APP to reboot into BOOT, then ERASE ¡ú DATA (1KB packets) ¡ú VERIFY (CRC32) ¡ú DONE, with progress and success dialogs. A standalone build exists at `TOOL/upgrader/dist/AT32Upgrader.exe` (PyInstaller `--onefile --windowed`, ~12.7MB); rebuild from the `TOOL/upgrader/` directory: `python -m PyInstaller --onefile --windowed --name AT32Upgrader upgrade_host.py`.

### Clean

Run `keilkilll.bat` from the **repo root** (it recursively deletes `*.o`, `*.d`, `*.crf`, `*.axf`, `*.map`, `*.dep`, `JLinkLog.txt`, etc.). It deliberately keeps `*.opt` to preserve J-Link settings.

### Lint / IDE IntelliSense

No linter or unit-test harness exists. `compile_flags.txt` (repo root) feeds clangd/LSP diagnostics with include paths and defines; it must mirror the `Software/APP/` prefix. Compiler warnings are effectively disabled in the Keil target (`-w`), so rely on the IDE diagnostics for static checking.

### Regenerate the Figma dashboard image

The dashboard background (`Application/app_ui_img_demo.c`, 115200 bytes of RGB565 pixel data) is tool-generated. Pipeline: Figma export in `Software/APP/assets/` (99 SVGs + PNG) ¡ú render `.codebuddy/figma/670_48/render.html` (relative-path copy of figma.html) at 240x240 in a browser and screenshot ¡ú run `python .codebuddy/figma/670_48/png_to_lvgl.py` to regenerate the C array (big-endian bytes, `LV_COLOR_16_SWAP=1`). Do not hand-edit pixel data.

## Architecture

### Layering

Strict one-way dependency order inside `Software/APP/`: `Application ¡ú Middleware ¡ú Drivers ¡ú Libraries`.

- **`Application/`** ¡ª business logic and task orchestration. `app_main.c/h` owns `main()`, clock init, and FreeRTOS task/queue creation; `app_ui.c/h` owns the LVGL demo UI and consumes events; `app_ui_img_demo.c/h` holds the tool-generated 240x240 gauge bitmap (`ui_img_demo_gauge`), drawn full-screen at the bottom layer of the dashboard.
- **`Middleware/`** ¡ª OS/graphics configuration and ports. `freertos/FreeRTOSConfig.h` (240MHz / 1kHz tick / 20KB heap), `lvgl/lv_conf.h` (16bpp, `LV_COLOR_16_SWAP=1`, `LV_USE_PERF_MONITOR=1` ¡ú FPS/CPU overlay bottom-right), `lvgl/lv_port_disp.c/h` (240x40 draw buffer + flush callback), `lvgl/lv_rtos_tick.h` (LVGL heartbeat bridge).
- **`Drivers/`** ¡ª peripheral drivers (filenames prefixed `driver_`), one pair per peripheral, mutually independent: `driver_lcd_st7789`, `driver_backlight` (TMR3 PWM), `driver_encoder` + `driver_encoder_accel`, `driver_lowpower`, `driver_sw_i2c`, `driver_sgp30`, `driver_aht20`, `driver_bmp280`, `driver_max30102` (heart rate/SpO2, 0x57), `driver_qmc6309` (magnetometer/compass, 0x1A), `driver_delay`, and `driver_at32f403a_407_it` (interrupt dispatch).
- **`Libraries/`** ¡ª vendor code: ArteryTek CMSIS + standard-peripheral library, FreeRTOS kernel, LVGL v8.3 source. Only the files listed in the uvprojx groups are compiled (the SPL subset excludes `at32f403a_407_pwc.c` on purpose ¡ª see power management).

Adjacent directories that are **compiled but inactive** or **not compiled at all**:
- `eezStudio/` (EEZ Studio-generated LVGL UI: `images.c`/`screens.c`/`styles.c`/`ui.c` ¡ª a separate `EEZStudio` group in the uvprojx) is compiled and linked, but `main()` never calls its entry; the live UI is `app_ui.c`. Treat it as a reserved alternate UI toolchain ¡ª don't delete, don't wire in accidentally.
- `GUINxp/` (NXP GUI Guider output incl. TTF fonts) and `Si3.5/` (Source Insight project) are referenced by no build group; historical tooling artifacts only.

Layering rules: drivers never call each other; `Application` never touches registers directly; LVGL APIs are only invoked from the LVGL task.

### Bootloader & firmware upgrade (dual-image system)

Flash partitioning: BOOT 0x08000000~0x08002FFF (12KB budget, actual ~4.7KB) | APP 0x08003000~0x0807F7FF (linked at 0x08003000 ¡ª the uvprojx IROM and `SCB->VTOR` in `main()` must stay in sync) | parameter page 0x0807F800 (last 2KB page: magic `BTP1`, app_size, app_crc32, flags `update_request`/`app_valid`).

BOOT decision at power-on: parameter page has `update_request` OR APP invalid (flag missing / stack pointer outside SRAM) OR PB3 held ¡ú USB upgrade mode; otherwise jump to APP (set VTOR/MSP, call reset vector, after `usb_cdc_deinit()` on the upgrade path only). The upgrade protocol (frames over USB CDC, see `Software/Shared/boot_shared.h`): HELLO ¡ú ERASE ¡ú DATA (1KB packets, ACK-sequenced) ¡ú VERIFY (bitwise CRC32 matching zlib) ¡ú DONE (write parameter page, jump). Flash writes only happen in main-loop/task context, never in USB ISRs ¡ª erase stalls instruction fetch.

USB stack: `Software/Shared/usb_cdc.c` is a hand-written compact CDC stack (the SPL usb/flash .c files are absent from the repo). AT32F403A has an F1-style USBFS device controller (NOT OTG): PA11/PA12, PMA at 0x40006000 with 16-bit data at 32-bit spacing, endpoints via BTABLE. USB clock comes from `crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_HICK)` (internal 48MHz, PLL-independent). EP0=control (32B packets), EP1=dual-direction bulk (CDC data), EP2=interrupt-in (notification, descriptor-only). `puo=1` enables the D+ pull-up (official `usbd_connect` semantics; `puo=0` disconnects ¡ª getting this backwards leaves the device invisible to the host). Writing EPxR must preserve both direction STAT fields ¡ª use `cdc_ept_write()`, not the stock SET_TXSTS/SET_RXSTS macros (they clear the other direction).

APP side: `app_usb_upgrade.c` keeps the CDC alive permanently (interrupt-driven, never touches LVGL/RTOS APIs; IRQ priorities 0xE/0xF are outside FreeRTOS's masked range). HELLO answers mode=APP; REBOOT sets a volatile flag handled in `encoder_task` ¡ª writes the parameter page, waits 100ms, `NVIC_SystemReset()`. This is what makes drag-and-drop upgrading work on a running device.

### FreeRTOS task model (thread-safety backbone)

| Task | Priority | Stack | Role |
|------|----------|-------|------|
| `encoder_task` | 3 | 1KB | 20ms poll of the encoder driver; posts `app_encoder_msg_t` to a queue; handles the 3s long-press shutdown sequence; checks the USB-upgrade reboot request each cycle. |
| `lvgl_task` | 2 | 4KB | Sole owner of LVGL: 5ms `lv_timer_handler()`; drains encoder/sensor/env/hr/compass queues and forwards to `app_ui_*` handlers. |
| `i2c_sensor_task` | 1 | 2.5KB | **Single owner of the software I2C bus** ¡ª polls all five bus devices in a 500ms-tick loop: MAX30102 FIFO read + HR/SpO2 algorithm first every tick (FIFO depth constraint, ~595ms steady interval), then QMC6309 heading read (A segment, 500ms), then SGP30 + AHT20 + BMP280 on every 2nd tick (1s period). Per-device ready flags with ~2s retry dividers support hot-plug. Posts to `g_sensor_queue` / `g_env_queue` / `g_hr_queue` / `g_compass_queue`. |
| idle | 0 | 512B | FreeRTOS built-in idle task. |

Because LVGL is not thread-safe, **all** UI access is confined to `lvgl_task`. Cross-thread data flows only through FreeRTOS queues (`g_encoder_queue`, `g_sensor_queue`, `g_env_queue`, `g_hr_queue`, `g_compass_queue`); their item structs use uniform `int32_t`/`uint32_t` fields to avoid mixed-width padding. Producers touch only drivers and queues, never LVGL. The software I2C bus (SGP30 + AHT20 + BMP280 + MAX30102 + QMC6309 on PA2/PA3) is non-thread-safe and now accessed **only** by `i2c_sensor_task` (single-task ownership); `g_i2c_mutex` (`configUSE_MUTEXES=1`) is still taken/given around each bus segment to keep concurrency safety if a future I2C consumer appears.

### Power management (the most subtle area)

**Current behavior**: `lowpower_init()` / `lowpower_boot_wait()` in `main()` are **commented out** (see `Application/app_main.c` Step1.1) ¡ª the board boots straight into the UI; no "press to power on" gate after reset. The stop-mode machinery itself is fully functional and still used by the long-press shutdown path:

- 3-second long-press (timed by TMR7 1ms ISR) triggers the shutdown sequence in `encoder_task`: post a message so the UI draws "Power off...", `backlight_off()` after 400ms, wait for release, then `lowpower_power_down()` (DEEPSLEEP/stop). Pressing the push button wakes via PB3 = `EXINT3` falling edge ¡ª execution resumes from `WFI` (not reset), the 240MHz PLL is re-enabled, and the previous screen is restored from retained RAM.
- A wakeup is only accepted if PB3 is held low for 50ms (debounce against contact-bounce "fake" falling edges); there is a 200ms quiet period before entering stop, and pending TMR7/SysTick/EXINT3 flags are cleared before `WFI`.
- Critical constraint: the post-wakeup clock restore must **never call `crm_reset()`** ¡ª during stop the CRM peripheral clock gates are preserved, and resetting them kills TMR7/SPI1/etc. `Drivers/driver_lowpower.c` therefore manipulates PWC register bit-fields directly (which is why the stock `at32f403a_407_pwc.c` is excluded from the build). A full STANDBY alternative also lives in `driver_lowpower.c` for future use.
- Re-enabling the boot-time "off" state is a matter of un-commenting the two calls in `main()`.

### LVGL / FreeRTOS integration specifics

- SysTick is taken over: `SysTick_Handler` calls `xPortSysTickHandler`; PendSV/SVC are redirected via `FreeRTOSConfig.h` macros.
- `LV_TICK_CUSTOM=1` reads `xTaskGetTickCount()` directly (1ms tick), so no `lv_tick_inc()` call sites exist; include-order bridging lives in `lv_rtos_tick.h`.
- Pixel format: `LV_COLOR_DEPTH 16` with `LV_COLOR_16_SWAP 1` ¡ª image data and the LCD flush path are big-endian RGB565. Any new bitmap assets must match this byte order.
- Dual-screen pages: push on the encoder toggles dash ? compass via `lv_scr_load_anim` (MOVE_LEFT/RIGHT, 250ms, `auto_del=FALSE` so both screens stay resident). The page-toggle button and the long-press power-off hint live on `lv_layer_top()` (visible on every page). The compass page uses `lv_meter` (`LV_USE_METER 1` in lv_conf.h + `extra/widgets/meter/lv_meter.c` added to the uvprojx); note this repo's meter API differs from stock v8.3: `lv_meter_set_scale_ticks(obj, scale, cnt, width, len, color)` (6 args) and `lv_meter_set_scale_range(obj, scale, min, max, angle_range, rotation)`.
- Memory: FreeRTOS heap_4 = 20KB (stacks/queues), LVGL TLSF = 32KB (UI objects), draw buffer = 240x40 (~18.8KB). Total RAM ~80KB/96KB; Flash ~183KB/1MB (the 115KB demo gauge bitmap is currently garbage-collected because the background-image code in `app_ui_init` is commented out ¡ª restoring it adds ~112KB).

### Key driver interfaces

- `backlight_init()` / `backlight_set_percent()` ¡ª TMR3_CH4 PWM on PB1, clamped 20Ã¿100%.
- `encoder_init()` / `encoder_get_delta()` / `encoder_push_pressed()` / `encoder_push_rising_edge()` / `encoder_push_hold_ms()` / `encoder_push_long_press()` ¡ª TMR7 1ms synchronous sampling + 4x state-table decode (jitter-immune); also maintains long-press duration and exposes live pin levels for the on-screen "A:x B:x P:x" self-check row.
- `encoder_accel_init()` / `encoder_accel_feed()` ¡ª maps raw delta to "steps" with slow-precise / fast-accelerating response; one reusable `encoder_accel_t` instance per use-case.
- `sgp30_init()` / `sgp30_read()` ¡ª software I2C (PA2/PA3, open-drain), `Init_air_quality` (0x2003) + `Measure` (0x2008) with CRC; ~15s warm-up showing 400ppm/0ppb is normal.
- `aht20_init()` / `aht20_read()` ¡ª AHT20 (0x38) temp/humidity, fixed-point x10 output (0.1 ¡«C / 0.1 %RH).
- `bmp280_init()` / `bmp280_read()` / `bmp280_altitude()` ¡ª BMP280 at address **0x77** (SDO floating/tied to VCC per `driver_bmp280.h`; use 0x76 only if SDO is grounded) ¡ª Bosch compensation, output 0.1 hPa fixed-point, altitude (m) via lookup + interpolation (ref 1015.7 hPa / 25 ¡«C, 8X/16X oversampling + IIR x16).
- `LCD_Init()` / `LCD_WriteAreaBytes()` ¡ª SPI1 init + window RGB565 writes used by the LVGL flush callback.
- `max30102_init()` / `max30102_read()` ¡ª MAX30102 heart-rate/SpO2 at 0x57 (Part ID 0x15), SpO2 mode (Red+IR, 100sps with SMP_AVE=2 ¡ú 50sps FIFO, 18-bit ADC). Driver embeds a fixed-point algorithm: 4.8s IR window ¡ú DC removal ¡ú 60%-height dynamic threshold peak detection ¡ú BPM (40¨C180, ¡Ý3 beats); SpO2 = 110 ? 25¡¤R with R = (ACred/DCred)/(ACir/DCir) via 64-bit integer math (70¨C100); finger detect via IR DC > 50000. Call period MUST stay ~500ms (FIFO depth 32 @ 50sps); overflow auto-recovers by resyncing RD=WR.
- `qmc6309_init()` / `qmc6309_read()` / `qmc6309_heading()` ¡ª QMC6309 magnetometer at **0x1A** (Chip ID 0x90; note this is a QST part, NOT the QMC5883L 0x0D despite the similar name). Init: soft-reset via Ctrl2 bit7 (self-locked, must be cleared by writing 0) ¡ú wait NVM-loaded (status 0x09 bit4) ¡ú Ctrl1=0x63 continuous mode ¡ú Ctrl2=0x28 (50Hz ODR + ¡À8 gauss). Read: status first (DRDY bit0 auto-clears on read, order matters) then 6 data bytes 0x01¨C0x06 little-endian signed X/Y/Z. `qmc6309_heading()` does runtime hard-iron calibration (min/max tracking per axis, center subtraction) + fixed-point 16-stage CORDIC atan2, returns 0.1¡ã units 0~3599 (N=0 clockwise); no tilt compensation (assumes level use). Adjust `QMC6309_HEADING_OFFSET` in the .c for mounting orientation.

### Hardware pin map (quick reference)

PB1 = backlight PWM; PB0/PA4/PA6 = LCD CS/DC/RES; PA5/PA7 = SPI1 SCK/MOSI; PB3 = encoder push (EXINT3, wake) / PB4-PB5 = encoder A/B (external pull-up); PA2/PA3 = shared software I2C (SGP30 + AHT20 + BMP280 + MAX30102 + QMC6309); PA11/PA12 = USB DM/DP (TypeC). PB3/PB4 are JTAG pins released to GPIO at `encoder_init` (SWD-only debug).
