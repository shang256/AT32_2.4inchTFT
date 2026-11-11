# -*- coding: utf-8 -*-
"""Generate render.html (relative asset paths + zero body margin) from figma.html"""
import pathlib

SRC = pathlib.Path(__file__).with_name("figma.html")
DST = pathlib.Path(__file__).with_name("render.html")

src = SRC.read_text(encoding="utf-8")
src = src.replace("file:///d%3A/shang256/AT32_2.4inchTFT/assets/CodeBuddyAssets/670_48/",
                  "/assets/CodeBuddyAssets/670_48/")
src = src.replace("file:///d:/shang256/AT32_2.4inchTFT/assets/CodeBuddyAssets/670_48/",
                  "/assets/CodeBuddyAssets/670_48/")

html = ('<!DOCTYPE html><html><head><meta charset="utf-8">'
        '<style>html,body{margin:0;padding:0}</style></head><body>'
        + src + '</body></html>')

DST.write_text(html, encoding="utf-8")
print("written:", DST)
