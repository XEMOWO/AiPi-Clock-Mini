#!/bin/bash
# WB2-clock 一键烧录 — 自动定位项目，调用项目里的 flash.sh
# 用法: ./一键烧录.sh   （可选参数同 flash.sh: --no-sync / --no-build / 串口 / 波特率）
cd "$(dirname "$0")/applications/xemowo/WB2-clock"
exec ./flash.sh "$@"
