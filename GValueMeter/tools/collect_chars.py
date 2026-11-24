#!/usr/bin/env python3
"""从源码里的字符串字面量提取所有非 ASCII 字符，作为字库字符集。"""
import re
import sys
import glob
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "chars_set.txt")

chars = set()
strlit = re.compile(r'"((?:[^"\\]|\\.)*)"')
inline_comment = re.compile(r'//[^\n]*')
block_comment = re.compile(r'/\*.*?\*/', re.S)

for f in sorted(glob.glob(os.path.join(ROOT, "*.cpp")) +
                glob.glob(os.path.join(ROOT, "*.h")) +
                glob.glob(os.path.join(ROOT, "*.ino"))):
    s = open(f, encoding="utf-8").read()
    s = block_comment.sub("", s)
    s = inline_comment.sub("", s)
    for m in strlit.finditer(s):
        body = m.group(1)
        for ch in body:
            if ord(ch) >= 0x80:
                chars.add(ch)

txt = "".join(sorted(chars))
open(OUT, "w", encoding="utf-8").write(txt)
print(len(txt))
print(txt)
