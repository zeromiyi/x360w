// vhid.h — IOHIDUserDevice 虚拟游戏手柄封装 (macOS 26)
// 需 AMFI 放行 (amfi_get_out_of_my_way=1) + ad-hoc 签名携带
// com.apple.developer.hid.virtual.device entitlement, 否则 create 返回 NULL。
//
// 双身份:
//   GENERIC   — 16键+Hat+6轴 通用手柄 (SDL/模拟器友好, GCController 不可见)
//   DUALSENSE — 完整克隆 DualSense (VID 054C PID 0CE6, 273字节官方描述符)
//               → GCController/系统设置/Unity 游戏可见 (实测 macOS 26.5 通过)
#ifndef X360W_VHID_H
#define X360W_VHID_H

#include <stdbool.h>
#include <stdint.h>

#define VHID_IDENTITY_GENERIC   0
#define VHID_IDENTITY_DUALSENSE 1

// 通用身份输入报告布局 (Report ID 1, 14 字节) — 仅 GENERIC 模式直接投递时用
#define VHID_INPUT_REPORT_LEN 14

typedef struct VPad VPad;

// 震动回调: strong=大马达 weak=小马达 (0-255)
typedef void (*VPadRumbleFn)(void *ctx, uint8_t strong, uint8_t weak);

// 创建虚拟手柄。identity: VHID_IDENTITY_*。失败返回 NULL 并填 err。
VPad *vpad_create(int slot, int identity, VPadRumbleFn on_rumble, void *rumble_ctx,
                  char *err, int err_len);

// 语义化投递: 由 vhid 层按身份翻译成对应报告格式。
// buttons 位序: 0=A 1=B 2=X 3=Y 4=LB 5=RB 6=Back 7=Start 8=L3 9=R3 10=Guide 11..14=十字U/D/L/R
// hat: 0..7 顺时针, 8=中位; 摇杆 s16 (HID 约定: Y 向下为正, 调用前已取反好)
bool vpad_submit_state(VPad *p, uint16_t buttons, uint8_t hat,
                       int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                       uint8_t lt, uint8_t rt);

// 销毁: 先送中性帧防按键卡死, 再 cancel。
void vpad_destroy(VPad *p);

#endif
