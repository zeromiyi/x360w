#!/bin/bash
# verify.sh — AMFI 放行后的 30 秒验证
cd "$(dirname "$0")/.."
echo "===== 1. 检查 boot-args ====="
if nvram -p 2>/dev/null | grep -q "amfi_get_out_of_my_way=1"; then
    echo "✅ boot-args 含 amfi_get_out_of_my_way=1"
else
    echo "❌ boot-args 未设置 —— 请按 INSTALL.md 进恢复模式设置后再来"
    exit 1
fi
echo "===== 2. 签名副本存在性 ====="
[ -x ./x360w-signed ] || { echo "❌ 缺 x360w-signed, 先跑 scripts/build.sh"; exit 1; }
echo "✅ x360w-signed 就绪"
echo "===== 3. 虚拟手柄探针 (30 秒, 左摇杆画圆) ====="
echo "   请同时打开: 系统设置 → 游戏手柄, 或浏览器 gamepad-tester.com"
./x360w-signed --hid-probe
rc=$?
echo "===== 结果 ====="
[ $rc -eq 0 ] && echo "🎉 AMFI 放行生效, 虚拟手柄可用! 日常使用: ./x360w-signed" \
              || echo "❌ 仍被拦截 —— 可能 26.5 已封堵该 boot-arg (见 INSTALL.md 回退与备选)"
exit $rc
