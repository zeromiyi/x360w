# x360w — Xbox 360 无线接收器 macOS 现代驱动

让 **Xbox 360 一代无线手柄**（配微软 PC 无线接收器 `045e:0291/0719/02a9`）
在 **macOS 13 → 26（Apple Silicon / Intel）** 上使用——无 kext、基础模式无需动 SIP。

它是已停更的 [360Controller](https://github.com/360Controller/360Controller)
内核驱动（2020-05 终版，仅支持到 macOS 10.15）的精神续作，按 2026 年 macOS
允许的方式重写：**libusb 用户态读 USB + IOHIDUserDevice 虚拟手柄**。

## 特性

- 🎮 完整手柄支持：按钮/十字键/摇杆/模拟扳机/接入断开/LED 象限，单接收器最多 4 只
- 🖥️ 双输出身份
  - **DualSense 克隆（默认）**：完整克隆索尼 DualSense（`054c:0ce6` + 官方 273 字节
    描述符）→ **GameController 框架 / 系统设置 / Unity 及原生 Mac 游戏可见**
  - **通用手柄**（`--identity generic`）：16 键 + Hat + 6 轴 → SDL/模拟器/浏览器
- 🔁 震动回传：HID 输出报告 → 真实马达
- 🪶 无 kext 无 DriverKit，单一小型 C 二进制，可 launchd 常驻
- 🔎 内置实时仪表盘（`-v` 抓包十六进制）

## 为什么克隆 DualSense？

macOS GameController 框架（系统设置 → 游戏手柄、Unity/原生游戏走的通道）
**刻意忽略用户态虚拟 HID 设备**。完整克隆一个苹果原生支持的真实手柄的身份与
描述符后，虚拟设备即成为一等公民——macOS 26.5 实测：系统设置显示已连接，
《潜水员戴夫》直接可玩（含震动）。游戏内显示 PS 键位图标（A=✕ B=○ X=□ Y=△）。

## 快速开始

```bash
scripts/build.sh     # 需 Xcode CLT + brew install libusb
./x360w              # USB 仪表盘模式, 零系统改动
```

要成为**系统级手柄**需一次性放行 AMFI 的 entitlement 强制检查（SIP 保持开启）：

```bash
# Apple Silicon: 关机 → 按住电源键 → 选项 → 终端:
nvram boot-args="amfi_get_out_of_my_way=1"
# 重启后:
./x360w-signed
```

> ⚠️ 放行后任何 ad-hoc 签名二进制都能携带受限 entitlement，请理解风险再操作；
> 恢复模式跑 `nvram -d boot-args` 可随时回退。
> 完整指南：[docs/INSTALL.zh-CN.md](docs/INSTALL.zh-CN.md)

开机自启：`cp packaging/com.x360w.driver.plist ~/Library/LaunchAgents/`
（先编辑其中的绝对路径）→ `launchctl load ...`

## 兼容性

| 消费端 | DualSense 模式 | 通用模式 |
|---|---|---|
| 系统设置 → 游戏手柄 | ✅ | ❌（Apple 过滤） |
| Unity / 原生 GCController 游戏 | ✅ | ❌ |
| Steam / SDL / OpenEmu / Dolphin | ✅ | ✅ |
| 浏览器 Gamepad API | ✅ | ✅ |
| 震动 | ✅ | ✅ |

实测：M4 Max · macOS 26.5.2 · 接收器 `045e:0719` · 原装 Xbox 360 手柄。

## 许可与致谢

MIT（见 [LICENSE](LICENSE)）。协议知识来自 Linux `xpad.c`；DualSense
描述符/协议来自 nondebug 的文档；AMFI 流程参考 Lumen；研究基础参考
X360MacOSReceiverBridge。完整致谢见 [NOTICE](NOTICE)。
