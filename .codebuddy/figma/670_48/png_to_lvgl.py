# -*- coding: utf-8 -*-
"""
Convert demo_240.png (240x240 RGB) into an LVGL v8 C image array (RGB565,
LV_COLOR_16_SWAP=1 => big-endian byte order), generating
Application/app_ui_img_demo.c and .h for the Keil project.
"""
import pathlib
from PIL import Image

ROOT = pathlib.Path(r"d:/shang256/AT32_2.4inchTFT")
PNG = ROOT / ".codebuddy/figma/670_48/demo_240.png"
OUT_C = ROOT / "Application/app_ui_img_demo.c"
OUT_H = ROOT / "Application/app_ui_img_demo.h"

img = Image.open(PNG).convert("RGB")
assert img.size == (240, 240), f"unexpected size {img.size}"
px = img.load()

lines = []
lines.append("/**")
lines.append("  **************************************************************************")
lines.append("  * @file    app_ui_img_demo.c")
lines.append("  * @brief   Dashboard demo gauge image (240x240 RGB565, LV_COLOR_16_SWAP=1)")
lines.append("  * @note    Pixel data is auto-generated from the Figma design screenshot;")
lines.append("  *          do not edit pixel data by hand")
lines.append("  **************************************************************************")
lines.append("  */")
lines.append("")
lines.append('#include "app_ui_img_demo.h"')
lines.append("")
lines.append("/* 240x240 = 57600 pixels, RGB565 2 bytes per pixel => 115200 bytes */")
lines.append("static const uint8_t img_demo_gauge_map[115200] = {")

per_line = 12  # bytes per line
chunk = []
for y in range(240):
    for x in range(240):
        r, g, b = px[x, y]
        rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        hi = (rgb565 >> 8) & 0xFF   # LV_COLOR_16_SWAP=1: high byte first
        lo = rgb565 & 0xFF
        chunk.append("0x%02X,0x%02X" % (hi, lo))

for i in range(0, len(chunk), per_line):
    lines.append("  " + ",".join(chunk[i:i + per_line]) + ",")

lines.append("};")
lines.append("")
lines.append("/* LVGL v8 image descriptor: TRUE_COLOR(RGB565), 240x240, opaque */")
lines.append("const lv_img_dsc_t ui_img_demo_gauge = {")
lines.append("  .header.cf = LV_IMG_CF_TRUE_COLOR,")
lines.append("  .header.always_zero = 0,")
lines.append("  .header.reserved = 0,")
lines.append("  .header.w = 240,")
lines.append("  .header.h = 240,")
lines.append("  .data_size = 240 * 240 * 2,")
lines.append("  .data = img_demo_gauge_map,")
lines.append("};")
lines.append("")

OUT_C.write_text("\n".join(lines), encoding="ascii")

header = """/**
  **************************************************************************
  * @file    app_ui_img_demo.h
  * @brief   Dashboard demo gauge image (240x240 RGB565) public interface
  * @note    Image data auto-generated from Figma design (see app_ui_img_demo.c)
  **************************************************************************
  */

#ifndef __APP_UI_IMG_DEMO_H
#define __APP_UI_IMG_DEMO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* Full-screen demo gauge image (240x240 RGB565, tool-generated, read-only) */
extern const lv_img_dsc_t ui_img_demo_gauge;

#ifdef __cplusplus
}
#endif

#endif /* __APP_UI_IMG_DEMO_H */
"""
OUT_H.write_text(header, encoding="ascii")

print("OK ->", OUT_C, OUT_H)
