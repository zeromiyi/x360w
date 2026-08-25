# x360w — Xbox 360 Wireless Receiver driver for modern macOS

Use your **original Xbox 360 wireless controller** (with the Microsoft PC Wireless
Gaming Receiver, `045e:0291` / `0719` / `02a9`) on **macOS 13 → 26, Apple Silicon
and Intel** — no kernel extension, no SIP disable required for the base mode.

It is the spiritual successor to the abandoned
[360Controller](https://github.com/360Controller/360Controller) kext driver
(final release May 2020, macOS 10.15 max), rewritten for how macOS works today:
**libusb userspace USB I/O + IOHIDUserDevice virtual gamepads**.

## Features

- 🎮 **Full controller support** — buttons, D-Pad, analog sticks, analog triggers,
  connect/disconnect, LED quadrant, up to 4 controllers per receiver
- 🖥️ **Two output identities**
  - **DualSense clone (default)** — a bit-exact Sony DualSense
    (`054c:0ce6`, official 273-byte USB descriptor). Visible to
    **GameController.framework / System Settings / Unity & native Mac games**.
  - **Generic gamepad** (`--identity generic`) — 16 buttons + hat + 6 axes.
    Best for SDL/emulators/browser Gamepad API.
- 🔁 **Rumble** — HID output reports are forwarded to the real motors
- 🪶 **No kext, no DriverKit** — one small C binary, `launchd`-friendly
- 🔎 **Built-in live dashboard** for debugging (`-v` for raw packet hex)

## Why a DualSense clone?

macOS GameController.framework (what System Settings → Game Controllers and
Unity/native games use) deliberately ignores user-space virtual HID devices.
Cloning the identity and descriptor of a **real, natively supported controller**
makes the virtual device a first-class citizen — verified on macOS 26.5 (Tahoe):
System Settings shows it as connected and Unity titles (e.g. DAVE THE DIVER)
are playable, rumble included. Games will show PlayStation glyphs
(A=✕ B=○ X=□ Y=△).

## Quick start

```bash
# 1. build (needs Xcode CLT + `brew install libusb`)
scripts/build.sh        # produces ./x360w and ./x360w-signed

# 2. USB-only dashboard — works with zero system changes
./x360w
```

For a **system-wide gamepad** macOS requires the managed entitlement
`com.apple.developer.hid.virtual.device`. The community-standard way for
personal use is to relax AMFI's entitlement enforcement **once**
(SIP stays enabled), then use the ad-hoc-signed binary:

```bash
# Apple Silicon: shut down → hold power → Options → Terminal:
nvram boot-args="amfi_get_out_of_my_way=1"
# reboot, then:
./x360w-signed
```

> ⚠️ This lets any ad-hoc-signed binary carry restricted entitlements.
> Understand the trade-off before applying; revert with `nvram -d boot-args`
> from the same recovery terminal. Full guide + rollback + alternatives:
> [docs/INSTALL.md](docs/INSTALL.md) · [中文文档](docs/INSTALL.zh-CN.md)

Daily use / autostart:

```bash
cp packaging/com.x360w.driver.plist ~/Library/LaunchAgents/  # edit path inside
launchctl load ~/Library/LaunchAgents/com.x360w.driver.plist
```

## Architecture

```
Xbox 360 Wireless Receiver (USB, FF/5D/81 × 4 slots)
   │  libusb interrupt transfers, one thread per slot
   ▼
Protocol decode (presence / input reports — Linux xpad.c wire format)
   │  buttons · dpad · sticks · triggers
   ▼
IOHIDUserDevice virtual gamepad (per connected controller)
   ├─ DualSense clone (default) → GameController / System Settings / Unity
   └─ Generic gamepad           → SDL / emulators / browser Gamepad API
   ▼
Rumble: HID output report → set-report callback → receiver rumble command
```

## Repo layout

```
src/        x360w.c (USB + protocol + dashboard) · vhid.c/h (virtual HID) · ds_desc.h
tests/      smoke & probe tools used to validate each layer (see docs)
scripts/    build.sh (compile + ad-hoc sign) · verify.sh (post-AMFI check)
packaging/  launchd agent plist
docs/       install guides (EN / 中文)
```

## Protocol cheatsheet (wireless)

- One USB interface per controller slot (`bInterfaceClass FF / SubClass 5D / Protocol 81`)
- IN: `data[0]&0x08` → presence change, `data[1]&0x80`=present `0x40`=headset;
  `data[1]==0x01` → input report starting at `data[4]` (wired-360 layout)
- OUT (12 B): LED `{00 00 08 40+mode}` · rumble `{00 01 0F C0 00 strong weak}`
  · presence query `{08 00 0F C0}` · power off `{00 00 08 C0}`

## Compatibility

| Consumer | DualSense mode | Generic mode |
|---|---|---|
| System Settings → Game Controllers | ✅ | ❌ (Apple filter) |
| Unity / native GCController games | ✅ | ❌ |
| Steam / SDL games / OpenEmu / Dolphin | ✅ | ✅ |
| Browser Gamepad API | ✅ | ✅ |
| Rumble | ✅ | ✅ |

Tested: M4 Max, macOS 26.5.2, receiver `045e:0719`, original Xbox 360 controller.

## License & credits

MIT (see [LICENSE](LICENSE)). Protocol knowledge from Linux `xpad.c`;
DualSense descriptor/protocol from @nondebug's documentation; AMFI flow
pioneered by Lumen; research groundwork by X360MacOSReceiverBridge.
Full attributions in [NOTICE](NOTICE).
