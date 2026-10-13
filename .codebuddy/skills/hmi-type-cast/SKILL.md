---
name: HMI Type Cast
description: >
  This skill should be used when reviewing or modifying code in this GD32F30x HMI firmware project
  that involves mixed types—assignment between uint32 and uint32_t, uint16_t vs uint8, enum to integer,
  integer to BOOL, coordinate macros to LCD parameters, or pointer type casting.
  Use it whenever type mismatch compiler warnings need to be resolved, or when adding new code that
  crosses type boundaries (e.g., assigning a uint32 runtime value to a uint8 struct field, passing an
  enum to a uint32 callback parameter, or using integer literals where an enum is expected).
  It enforces consistent explicit C-style casts following project conventions.
---

# HMI Type Cast — GD32F30x Firmware 类型强制转换规范

## Purpose

Ensure every type-mismatched expression in this GD32F30x HMI firmware project uses an
explicit C-style cast that matches the project's established patterns, eliminating
all compiler warnings without suppressing them.

## When to Use

Trigger this skill in these situations:

- Reviewing or writing code where a `uint32` value is assigned to a `uint8`/`uint16` field
- Passing an integer literal (`0`, `1`) to a function expecting an enum type
- Calling `LCD_DrawBmpByID()` — coordinate arguments always need `(uint16)` cast
- Using `paramCB.runtime.*` or `paramCB.nvm.param.*` values in assignments where the
  left-hand side has a different type (narrower integer, different signedness, enum)
- Assigning boolean/integer to `BOOL` typed fields
- Passing `uint32` callback parameter (`ulBuff`) as a condition or enum in switch/ternary
- Mixing `uint32` (== `unsigned long`) and `uint32_t` (== `unsigned int`) in the same expression
- Buffer/string operations involving `uint8*`, `char*`, `const uint8*` pointer casts

## Type System Reference

Detailed type definitions, common cast patterns with real code examples, and a
frequency summary are maintained in `references/type-cast-guide.md`.
Load this reference when exact typedef mappings or pattern examples are needed.

### Quick Cheat Sheet

| Source Type | Target Type | Project Cast Pattern |
|-------------|-------------|---------------------|
| `int` literal coords | `uint16` | `(uint16)UI_ITEM_COODINATE_XXX_X` |
| integer literal / expression | `PIC_ID_E` | `(PIC_ID_E)(PIC_ID_BASE + offset)` |
| `uint32 ulBuff` | `BOOL` condition | `(BOOL)ulBuff ? ... : ...` |
| `uint32 ulBuff` | `DISPLAY_ITEM_E` / `UNIT_E` | `(UNIT_E)ulBuff` in switch |
| `int` literal `0`, `1` | `uint8` struct field | `param.field = (uint8)1;` |
| `uint16` / `uint32` → `uint8` | | `(uint8)value` |
| `paramCB.runtime.*` (uint32) | `MENU_NOMAL_E` / enum | `(MENU_NOMAL_E)PARAM_GetXxx()` |
| `uint32` / `int` → `BOOL` | | `(BOOL)PARAM_GetXxx()` |
| unsigned → `(uint8*)` / `(char*)` | | `(uint8*)buf` / `(char*)buf` |
| `uint32` function return | `uint16` | `return (uint16)temp;` |
| enum ← integer loop var | | `i = (DISPLAY_ITEM_E)0;` |

## Rules

### 1. Always Cast Narrowing Assignments

When a wider type (uint32, uint16, int) is assigned to a narrower field (uint8, enum),
insert an explicit cast. The compiler may warn about implicit truncation even when the
actual value fits.

```c
// Required: add cast when default-initializing uint8 fields
paramCB.nvm.param.common.uartLevel   = (uint8)1;
paramCB.nvm.param.protocol.resFactorySet = (uint8)1;

// Required: add cast for uint32 → uint8 runtime assignments
paramCB.runtime.hudnav.direction = (uint8)0;
paramCB.runtime.revCanAck = (uint8)0;
```

### 2. Cast Integer Literals to Enums

Integer literals (especially `0` as initial value) assigned to enum-typed variables
or struct fields require explicit enum casts.

```c
// Required
i = (DISPLAY_ITEM_E)0;
displayStateCB.menuNomalItem.menuNomalIndex = (MENU_NOMAL_E)0;

// Required: switch on uint32 callback parameter
switch ((UNIT_E)ulBuff) { ... }

// Required: enum from expression
SPEED_LIMIT_LEVEL_ID_E level = (SPEED_LIMIT_LEVEL_ID_E)(ulBuff & 0xFF);
```

### 3. Cast BOOL Values Explicitly

All assignments to `BOOL`-typed fields or uses of integer values in boolean context
require explicit `(BOOL)` cast.

```c
// Required
displayStateCB.menuNomalItem.cadenceLimted = (BOOL)paramCB.nvm.param.protocol.driver.H6107.cadenceModeSwitch;
displayStateCB.menuNomalItem.unitSwitch = (BOOL)PARAM_GetUnit();

// Required: ternary with uint32 callback param
(BOOL)ulBuff ? LCD_OPERATOR_MODE_DRAW : LCD_OPERATOR_MODE_ALL_BLACK

// Required: logical negation before casting
BOOL bLigth = (BOOL)(!(PARAM_GetLightSwitch()));
```

### 4. Cast Coordinate Arguments to (uint16)

Every `LCD_DrawBmpByID()` call takes `uint16` for X and Y. All coordinate macros
(`UI_ITEM_COODINATE_*`) produce `int` literals and must be cast.

```c
// Required pattern — the (uint16) cast on x and y is mandatory:
LCD_DrawBmpByID((uint16)UI_ITEM_COODINATE_BAT_ICON_X, (uint16)UI_ITEM_COODINATE_BAT_ICON_Y,
                PIC_ID_BATTERY_0, LCD_OPERATOR_MODE_DRAW);
```

### 5. Cast Pointer Types for String/Buffer Operations

When passing byte buffers to string functions or vice versa, use explicit pointer casts.

```c
// Required
strcpy((char *)paramCB.nvm.param.protocol.driver.SW_CONTROL_WLG_VERSION,
       (const char *)SW_CONTROL_VERSION_DATA);

SPI_FLASH_WriteWithErase((uint8*)SW_WLG_VERSION_DATA, SPI_FLASH_APP_ADDEESS + 1, length);

STRING_Find((const uint8 *)buff, (const uint8 *)UI_WLG_VERSION_DATA_RULE)
```

### 6. Cast Arithmetic Operands Before Narrowing Operations

When a wider type participates in arithmetic with bitmask truncation, cast before
the operation to make intent clear.

```c
// Good: cast before bitmask
SPEED_LIMIT_LEVEL_ID_E level = (SPEED_LIMIT_LEVEL_ID_E)(ulBuff & 0xFF);

// Good: float conversion for division
fTempTrip = (float)paramCB.runtime.speed * PARAM_GetParamCycleTime() / 3600000.f;
```

### 7. No (void) Suppression of Unused Parameters

This project does NOT use `(void)variable` casts to suppress "unused parameter"
warnings. The compiler is configured to tolerate unused callback parameters in
the unified `uint32 ulBuff` / `uint32 param` signature pattern. Do not introduce
`(void)` casts for this purpose.

## Workflow

When asked to fix type inconsistencies or eliminate compiler warnings:

1. **Identify**: Scan the modified/affected code for assignments, function calls,
   and expressions where the source and destination types differ.

2. **Determine the correct cast**: Match against the Quick Cheat Sheet above.
   If the pattern is not covered, load `references/type-cast-guide.md` for
   additional examples.

3. **Apply the cast**: Use C-style casts only — `(TargetType)expression`.
   Never introduce C++-style casts (`static_cast<>`, `reinterpret_cast<>`).

4. **Preserve parentheses**: When casting an expression (not a single variable),
   wrap the expression in parentheses: `(PIC_ID_E)(PIC_ID_BASE + offset)`.

5. **Do not change logic**: Only insert casts; never alter the semantics or
   control flow of existing code.

6. **Check enum validity**: When casting to an enum type, verify the value is
   a valid enum member or falls within the enum range defined in the header.
