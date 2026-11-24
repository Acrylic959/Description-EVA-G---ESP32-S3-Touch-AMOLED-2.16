#!/bin/bash
# 桌面模拟器：编译固件里与设备无关的部分，跑一段合成驾驶，输出 PNG
set -e
cd "$(dirname "$0")/.."
mkdir -p sim/out
python3 sim/gen_font.py >/dev/null
g++ -std=c++17 -O2 -DGVM_SIM -Isim/stubs -I. -o sim/simrun \
    sim/sim_main.cpp gmeter.cpp pet_mode.cpp pro_mode.cpp gg_mode.cpp cjk_font.cpp -lz
./sim/simrun sim/out
echo "截图在 sim/out/"
