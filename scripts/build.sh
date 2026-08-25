#!/bin/bash
# build.sh — 编译 x360w 并生成带 entitlement 的 ad-hoc 签名副本
# 产物: x360w (未签名, AMFI 未放行时也能跑 USB 仪表盘)
#       x360w-signed (ad-hoc + hid.virtual.device, AMFI 放行后使用)
set -e
cd "$(dirname "$0")/.."

clang -O2 -Wall -o x360w src/x360w.c src/vhid.c \
    -I/opt/homebrew/include -L/opt/homebrew/lib -lusb-1.0 -lpthread \
    -framework CoreFoundation -framework IOKit
echo "[build] x360w 编译完成"

cp x360w x360w-signed
codesign -s - --entitlements entitlements.plist -f x360w-signed
echo "[build] x360w-signed 签名完成 (ad-hoc + com.apple.developer.hid.virtual.device)"
codesign -d --entitlements - x360w-signed 2>&1 | grep -A2 "hid.virtual" || true
