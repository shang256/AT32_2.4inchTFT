---
name: HMI Code Commenter
description: |
  This skill should be used whenever generating new code, modifying existing code,
  or adding functions/variables/macros/enums in this GD32F30x HMI firmware project.
  It ensures all new code follows the project's comment standards and code style rules:
  Chinese-style function headers (函数名/入参/出参/返回值/功能描述/修改历史), step-by-step inline markers,
  macro documentation, and mandatory braces for all control flow statements.
---

# HMI Code Commenter

Add comments to newly generated code and enforce code style rules following this project's conventions.

## When to Use

Trigger this skill whenever:

- Adding a new function (any file)
- Adding a new macro, enum value, or struct field
- Adding a new case branch in a switch statement
- Adding a new if/for/while control flow statement
- Modifying any existing function body with new logic
- Creating a new `.c` or `.h` file

## Code Style Rules

### If / else if / else — Mandatory Braces

All if, else if, and else blocks MUST use braces `{}`, and the opening brace MUST be on a **new line**:

```c
if (condition)
{
    // body
}
else if (other)
{
    // body
}
else
{
    // body
}
```

This applies to single-line bodies as well — never omit braces:

```c
// WRONG — no braces, same-line brace:
if (x) return;
if (x) { return; }

// CORRECT:
if (x)
{
    return;
}
```

### For / While Loops

Same rule — braces on new line, always required:

```c
for (i = 0; i < MAX; i++)
{
    // body
}

while (condition)
{
    // body
}
```

## Comment Standards

### 1. Function Comment Header

For **new functions** or **modified existing functions**, use the project's Chinese-style header block:

```c
/*********************************************************************
 * 函 数 名: MODULE_FunctionName
 * 函数入参: uint8 param  参数含义说明
 * 函数出参: 无
 * 返 回 值: void
 * 功能描述: 一句话概括函数功能
 * 说明: 补充说明（设计意图、像素尺寸、调用条件等，可选）
 ***********
 * 修改历史:
 *   1.修改作者: sjh
 *     修改日期: 2026年07月04日
 *     修改描述: 新函数
 **********************************************************************/
void MODULE_FunctionName(uint8 param)
```

For **modifying existing functions**, append a new entry to the `修改历史` section without changing the existing entries.

For detailed field requirements, refer to the `HMI Function Comment` skill.

### 2. Inline Step Comments

Inside function bodies, separate major steps with:

```c
/* ---- Step1: Description of first action ---- */
/* ---- Step2: Description of second action ---- */
```

For large code sections, use equals-style separators:

```c
/* ========== Section Name ========== */
```

### 3. Variable and Field Comments

- Local variables: single-line `//` comment after declaration
- Struct fields: inline `//` comment to the right
- Enum values: no individual comments needed if naming is clear

```c
uint16 uwSpeed;      // 实时速度, 单位: 0.1 km/h
BOOL cruiseState;    // 当前巡航状态, 0:静止; 1:正在巡航
```

### 4. Macro Comments

Every new macro MUST have a preceding comment line:

```c
// 设置Xxx状态, 0:xxx; 1:xxx
#define PARAM_SetXxxState(v) paramCB.runtime.xxxState = v
// 获取Xxx状态
#define PARAM_GetXxxState()  (paramCB.runtime.xxxState)
```

### 5. Switch-Case Comments

Each case should have a label comment:

```c
switch (cmd)
{
/* ---- TTM:CONNECT_UUID — device connect callback ---- */
case BLE_PROTOCOL_TTM_HOST_CMD_CONNECT_UUID:
    ...
    break;
}
```

### 6. If-Block Comments for Device-Specific Logic

When the same command handler dispatches to multiple device types, add a comment above each if-block:

```c
/* heart rate device: parse connect state, update UI, start battery read */
if (TTM_IsTargetHeartRateDevice(pCmdFrame))
{
    TTM_ProcessHeartRateConnectEvent(pCmdFrame);
}
/* K31W wireless button: parse connect state, send auth command */
if (TTM_IsTargetK31WDevice(pCmdFrame))
{
    TTM_ProcessK31WConnectEvent(pCmdFrame);
}
```

### 7. New UI Element Pattern

When adding a display item, include a summary comment block in the draw function:

```c
 * 功能描述: 绘制/清除状态栏上的 XXX 图标
 *           [背景] PIC_ID_XXX_BG    (x, y)
 *           [图标] PIC_ID_XXX      (x, y)
```

## Comment Language

- Function headers and section separators: **Chinese** (GBK encoding)
- Descriptive text in inline comments: **Chinese**
- Variable/field inline comments: **Chinese**
- Struct field comments: **Chinese** (inline `//` style)

## Anti-patterns to Avoid

- Do NOT leave empty `else {}` blocks — either remove or add a comment
- Do NOT write misleading comments (e.g., "loop upper bound -1" when loop goes to MAX)
- Do NOT copy-paste comments from similar code without updating the actual behavior
- Do NOT skip comments for "obvious" code — the codebase standard requires them
- Do NOT omit braces on single-line if/for/while — braces are always mandatory
- Do NOT place opening brace on same line — always put `{` on a new line
