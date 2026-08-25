# 安装与 AMFI 放行指南（中文）

适用：macOS 13 → 26（Apple Silicon / Intel）。已实测：M4 Max + macOS 26.5.2。

## 1. 编译

```bash
brew install libusb          # Apple Silicon 用 /opt/homebrew/bin/brew
scripts/build.sh
```

产物：
- `x360w` —— 未签名，纯 USB 仪表盘模式（不改任何系统设置）
- `x360w-signed` —— ad-hoc 签名并携带 `com.apple.developer.hid.virtual.device`

## 2. 为什么需要放行 AMFI

在 macOS 26 上创建虚拟 HID 手柄（`IOHIDUserDevice`）需要 Apple 托管授权
`com.apple.developer.hid.virtual.device`（正常渠道：$99 开发者账号 + 工单审批）。
个人自用走社区标准做法：一次性放行 AMFI 的 entitlement 强制检查，**SIP 保持开启**。

> ⚠️ 安全影响：放行后任何 ad-hoc 签名二进制都能携带受限 entitlement
> （恶意软件理论上也能注入虚拟输入设备）。Gatekeeper、系统卷只读等其余
> SIP 保护全部仍在。不能接受就只用 `./x360w`（无虚拟手柄），或走苹果官方申请。

### 操作（约 3 分钟，一次性）

1. **关机**（不是重启）
2. Apple Silicon：按住**电源键**直到"正在载入启动选项"
3. **选项** → **继续**
4. **实用工具 → 终端**：

   ```
   nvram boot-args="amfi_get_out_of_my_way=1"
   ```

5.  → **重新启动**

若你的机器 SIP 本来就是关的，可跳过 1-4，直接在正常系统里
`sudo nvram boot-args="amfi_get_out_of_my_way=1"`。

### 回退（随时）

恢复模式终端：`nvram -d boot-args`

## 3. 验证（30 秒）

```bash
scripts/verify.sh
```

检查 boot-args 并跑虚拟手柄探针（左摇杆画圆 30 秒）。成功时
**系统设置 → 游戏手柄**出现手柄，gamepad-tester.com 能看到摇杆画圆。

## 4. 日常使用

```bash
./x360w-signed                       # 完整模式: USB + 虚拟手柄 + 震动
./x360w-signed --identity generic    # 通用手柄身份 (SDL/模拟器场景)
./x360w-signed -v                    # 抓包十六进制
./x360w --no-hid                     # 纯仪表盘
```

配对：按接收器按钮（灯闪）→ 长按手柄顶部 Connect 键 → 左上象限常亮。

开机自启：

```bash
# 先编辑 packaging/com.x360w.driver.plist 里的绝对路径
cp packaging/com.x360w.driver.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.x360w.driver.plist
```

## 排障

| 症状 | 原因/处理 |
|---|---|
| `x360w-signed` 启动即 Killed: 9 | AMFI 未放行或 boot-args 未持久化，重做第 2 步 |
| 探针报"IOHIDUserDevice 创建被拒" | 同上 |
| 找不到接收器 | `system_profiler SPUSBDataType` 查 `045e`；换 USB-A→C 转接/Hub |
| 配对了但游戏看不到 | 你处在 generic 模式，请用默认 DualSense 模式 |
| 系统设置里显示"未连接" | 驱动没在跑；启动 `./x360w-signed` 即转为已连接 |
| 系统升级后失效 | 回查 `nvram boot-args`；重跑 `scripts/build.sh` |

若苹果未来彻底封堵 `amfi_get_out_of_my_way`（26.4 beta 有报告，26.5.2 仍可用），
备选：苹果官方 entitlement 申请，或仅服务模拟器的 DSU/cemuhook 输出模式。
