# Type Cast Reference ！ GD32F30x HMI Firmware

## Exact Type Definitions (from `User/common.h`)

### Primary Project Types (line ~130-154, common.h)

| Type | C Type | Width | Signed |
|------|--------|-------|--------|
| `uint8` | `unsigned char` | 8-bit | unsigned |
| `uint16` | `unsigned short` | 16-bit | unsigned |
| `uint32` | `unsigned long` | 32-bit | unsigned |
| `int8` | `signed char` | 8-bit | signed |
| `int16` | `signed short` | 16-bit | signed |
| `int32` | `signed long` | 32-bit | signed |
| `uint64` | `unsigned long long` | 64-bit | unsigned |
| `fp32` | `float` | 32-bit | ！ |
| `fp64` | `double` | 64-bit | ！ |

### stdint Compatibility Types (line ~146-153, common.h)

| Type | C Type | Note |
|------|--------|------|
| `uint8_t` | `unsigned char` | Same as `uint8` |
| `uint16_t` | `unsigned short int` | Same width as `uint16` |
| `uint32_t` | `unsigned int` | **Different from `uint32`** (int vs long) |
| `int8_t` | `signed char` | Same as `int8` |
| `int16_t` | `signed short int` | Same width as `int16` |
| `int32_t` | `signed int` | **Different from `int32`** (int vs long) |

### Short Aliases (line ~310-320, common.h)

| Type | C Type |
|------|--------|
| `u8` | `unsigned char` |
| `u16` | `unsigned short int` |
| `u32` | `unsigned int` |
| `s8` | `signed char` |
| `s16` | `signed short int` |
| `s32` | `signed int` |

### BOOL (line ~174-179, common.h)

```c
typedef enum { FALSE = 0, TRUE = 1, OFF = 0, ON = 1 } BOOL;
```

### NULL/Invalid Value Macros (line ~162-164, common.h)

```c
#define UINT32_NULL  0xFFFFFFFF
#define UINT16_NULL  0xFFFF
#define UINT8_NULL   0xFF
```

---

## Key Type Mismatch: uint32 vs uint32_t

**CRITICAL**: `uint32` = `unsigned long` while `uint32_t` = `unsigned int`.
On ARM Cortex-M4 (32-bit), both are 4 bytes ！ no data loss ！ but mixing them
without explicit cast may trigger "incompatible types" or "sign mismatch" warnings.

**Rule**: When assigning between `uint32` and `uint32_t` (or passing as arguments),
always add an explicit cast to the destination type.

---

## Common Cast Patterns ！ Real Code Examples

### Pattern A: Coordinate Casts for LCD Functions

The most frequent cast in the entire project. Every `LCD_DrawBmpByID()` call
requires `(uint16)` on X and Y because coordinate macros are `int` literals.

```c
// Source: display.c ~line 279
LCD_DrawBmpByID((uint16)UI_ITEM_COODINATE_BAT_ICON_X,
                (uint16)UI_ITEM_COODINATE_BAT_ICON_Y,
                (PIC_ID_E)(PIC_ID_BATTERY_0),
                LCD_OPERATOR_MODE_DRAW);
```

```c
// Source: display.c ~line 463
LCD_DrawBmpByID((uint16)UI_ITEM_COODINATE_S_DOT_X,
                (uint16)UI_ITEM_COODINATE_S_DOT_Y,
                PIC_ID_HP_SPEED_NUM_DOT,
                LCD_OPERATOR_MODE_DRAW);
```

```c
// Source: display.c ~line 187 (computed coordinates)
LCD_DrawBmpByID((uint16)x, (uint16)y,
                (PIC_ID_E)ulBuff,
                LCD_OPERATOR_MODE_DRAW);
```

### Pattern B: PIC_ID_E Enum Cast for Image IDs

Image ID arithmetic must be cast to `PIC_ID_E`:

```c
// Source: display.c ~line 318
(PIC_ID_E)(PIC_ID_BATTERY_NUM_0 + value3)

// Source: display.c ~line 468
(PIC_ID_E)(PIC_ID_HP_SPEED_NUM_S_0 + value3)

// Source: display.c ~line 381
(PIC_ID_E)(PIC_ID_ICON_USB)
```

### Pattern C: BOOL Cast from uint32 Parameter

The unified callback signature `void Fn(uint32 ulBuff)` requires BOOL cast
when using the parameter as a condition:

```c
// Source: display.c ~line 381
(BOOL)ulBuff ? LCD_OPERATOR_MODE_DRAW : LCD_OPERATOR_MODE_ALL_BLACK

// Source: display.c ~line 399
(BOOL)ulBuff ? LCD_OPERATOR_MODE_DRAW : LCD_OPERATOR_MODE_ALL_BLACK
```

### Pattern D: BOOL Cast for NVM/Runtime Value Assignment

```c
// Source: state.c ~line 329
displayStateCB.menuNomalItem.cadenceLimted =
    (BOOL)paramCB.nvm.param.protocol.driver.H6107.cadenceModeSwitch;

// Source: state.c ~line 356
displayStateCB.menuNomalItem.unitSwitch = (BOOL)PARAM_GetUnit();

// Source: state.c ~line 538
BOOL bLigth = (BOOL)(!(PARAM_GetLightSwitch()));
```

### Pattern E: uint8 Cast for Field Initialization

```c
// Source: Param.c ~line 276-282
paramCB.nvm.param.common.uartLevel   = (uint8)1;
paramCB.nvm.param.common.beepSwitch  = (uint8)1;
paramCB.nvm.param.protocol.driver.cruiseSwitch = (uint8)0;
paramCB.nvm.param.protocol.resFactorySet       = (uint8)1;

// Source: Param.c ~line 313
paramCB.nvm.param.protocol.dp.gymMode = (uint8)0;

// Source: Param.c ~line 708
paramCB.runtime.hudnav.direction = (uint8)0;

// Source: Param.c ~line 763-766
paramCB.runtime.wlgCanRecv.battaryOutputState = (uint8)0;
paramCB.runtime.wlgCanRecv.transferModeFlag   = (uint8)0;
paramCB.runtime.revCanAck                     = (uint8)0;
```

### Pattern F: Enum Cast for Loop Variables and Index Resets

```c
// Source: display.c ~line 148
for (i = (DISPLAY_ITEM_E)0; i < DISPLAY_MAX; i++)

// Source: state.c ~line 161-163
displayStateCB.menuNomalItem.menuNomalIndex = (MENU_NOMAL_E)0;
displayStateCB.menuNomalItem.menudIndexUp   = (MENU_NOMAL_E)0;
displayStateCB.menuNomalItem.menudIndexDown = (MENU_NOMAL_E)3;
```

### Pattern G: Enum Cast for Switch on uint32 Parameter

```c
// Source: display.c ~line 496
switch ((UNIT_E)ulBuff) {
    // ...
}
```

### Pattern H: Enum Cast from PARAM_GetXxx Return Value

```c
// Source: state.c ~line 263
displayStateCB.menuNomalItem.controlSensor =
    (CONTROL_SENSOR_E)PARAM_GetControlSenonMode();

// Source: state.c ~line 181
BACK_LIGHT_Start((BACK_LIGHT_LEVEL_E)PARAM_GetBrightness());

// Source: state.c ~line 529
PARAM_SetAssistLevel((ASSIST_ID_E)uwAssist);
```

### Pattern I: Bitmask + Enum Expression Cast

```c
// Source: display.c ~line 680
SPEED_LIMIT_LEVEL_ID_E level = (SPEED_LIMIT_LEVEL_ID_E)(ulBuff & 0xFF);
```

### Pattern J: Float Cast for Division (Prevent Integer Truncation)

```c
// Source: Param.c ~line 878
fTempTrip = (float)paramCB.runtime.speed * PARAM_GetParamCycleTime() / 3600000.f;

// Source: Param.c ~line 914
fTempAvgSpeed = (float)paramCB.nvm.param.common.record.trip.distance * 3600.0f / ...;
```

### Pattern K: Return Value Narrowing Cast

```c
// Source: Param.c ~line 1398
return (uint16)temp;
```

### Pattern L: Pointer Casts for String/Buffer Operations

```c
// Source: Param.c ~line 355 ！ strcpy with byte buffer
strcpy((char *)paramCB.nvm.param.protocol.driver.SW_CONTROL_WLG_VERSION,
       (const char *)SW_CONTROL_VERSION_DATA);

// Source: Param.c ~line 549 ！ string compare
STRING_CmpareN((const uint8*)buff, (const uint8*)SW_WLG_VERSION_DATA, length);

// Source: Param.c ~line 551 ！ SPI flash write
SPI_FLASH_WriteWithErase((uint8*)SW_WLG_VERSION_DATA,
                         SPI_FLASH_APP_ADDEESS + 1, length);

// Source: Param.c ~line 616 ！ string find
STRING_Find((const uint8 *)buff, (const uint8 *)UI_WLG_VERSION_DATA_RULE);
```

---

## Anti-Patterns ！ Do NOT Use

1. **Do NOT use `(void)variable` casts** to suppress unused-parameter warnings.
   This project tolerates unused `uint32 ulBuff` callback parameters.

2. **Do NOT use C++-style casts**: No `static_cast<>`, `reinterpret_cast<>`,
   `const_cast<>`, or `dynamic_cast<>`. C-style casts only.

3. **Do NOT wrap macros**: Do not create `TO_UINT16(x)` or similar wrapper
   macros unless explicitly approved. Use direct `(type)value` casts.

4. **Do NOT cast away const silently**: If a function takes `const uint8*`,
   keep the `const` in the cast: `(const uint8*)ptr`.

5. **Do NOT add casts when types already match**: Adding unnecessary casts
   clutters the code. Only cast when the compiler would warn.
