# Installation & AMFI guide

Target: macOS 13 → 26 (Apple Silicon & Intel). Verified on M4 Max / macOS 26.5.2.

## 1. Build

```bash
brew install libusb          # ARM Homebrew: /opt/homebrew/bin/brew
scripts/build.sh
```

Produces:
- `x360w` — unsigned, USB-dashboard mode (no system changes needed)
- `x360w-signed` — ad-hoc signed with `com.apple.developer.hid.virtual.device`

## 2. Why AMFI relaxation is needed

Creating virtual HID gamepads (`IOHIDUserDevice`) on modern macOS requires the
**managed entitlement** `com.apple.developer.hid.virtual.device`, normally only
granted by Apple through a provisioning profile (paid developer account +
manual approval). For personal use, the community standard is to relax AMFI's
entitlement enforcement once. **SIP stays enabled.**

> ⚠️ Security trade-off: afterwards, any ad-hoc-signed binary may carry
> restricted entitlements (a malicious binary could also synthesize input
> devices). Gatekeeper, signed-system-volume and all other SIP protections
> remain intact. If that is unacceptable, use `./x360w` (USB dashboard only,
> no virtual gamepad) or request the entitlement from Apple instead.

### Apply (≈3 min, one-time)

1. **Shut down** (not restart)
2. Apple Silicon: hold the **power button** until "Loading startup options"
3. **Options** → **Continue**
4. **Utilities → Terminal**:

   ```
   nvram boot-args="amfi_get_out_of_my_way=1"
   ```

5.  → **Restart**

If SIP is already disabled on your machine, step 1-4 can be replaced by a
normal-macOS admin command: `sudo nvram boot-args="amfi_get_out_of_my_way=1"`.

### Revert (any time)

Same recovery terminal: `nvram -d boot-args`

## 3. Verify (30 s)

```bash
scripts/verify.sh
```

Checks boot-args, then runs a virtual-gamepad probe (left stick circles for
30 s). Success ⇒ System Settings → Game Controllers shows a controller and
[gamepad-tester.com](https://gamepad-tester.com) shows movement.

## 4. Daily use

```bash
./x360w-signed                 # full mode: USB + virtual gamepad(s) + rumble
./x360w-signed --identity generic   # generic gamepad identity (SDL/emulators)
./x360w-signed -v              # raw packet hex dump
./x360w --no-hid               # dashboard only
```

Pairing: press the receiver's button (LED blinks) → hold the controller's
top **Connect** button → quadrant 1 lights up solid.

Autostart on login:

```bash
# edit the absolute path inside packaging/com.x360w.driver.plist first
cp packaging/com.x360w.driver.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.x360w.driver.plist
```

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `x360w-signed` killed instantly (Killed: 9) | AMFI not relaxed yet, or boot-args not persisted — redo step 2 |
| probe: "IOHIDUserDevice 创建被拒" | same as above |
| receiver not found | check `system_profiler SPUSBDataType` for `045e`; try a different USB-A→C adapter/hub |
| controller pairs but no game sees it | you are in `--identity generic` mode; use default DualSense mode |
| System Settings shows controller "not connected" | happens only when the driver isn't running; start `./x360w-signed` |
| macOS update breaks it | re-check `nvram boot-args`; re-run `scripts/build.sh` |

If Apple ever blocks `amfi_get_out_of_my_way` entirely (reported on a 26.4
beta, still working on 26.5.2), remaining options: request the entitlement
from Apple ($99 dev account + approval), or a DSU/cemuhook output mode for
emulators only.
