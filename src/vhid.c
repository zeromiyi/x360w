// vhid.c — 见 vhid.h。API 符号全部 dlsym 动态绑定 (CLT SDK 无 IOHIDUserDevice.h)。
// 回调签名来源: opensource-apple/IOKitUser hid.subproj/IOHIDUserDevice.h (苹果官方头文件)
// DualSense 描述符/报告格式: github.com/nondebug/dualsense 协议文档 (273 字节官方 USB 描述符)
#include "vhid.h"
#include "ds_desc.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOReturn.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct __IOHIDUserDevice *IOHIDUserDeviceRef;
typedef IOHIDUserDeviceRef (*CreateFn)(CFAllocatorRef, CFDictionaryRef, uint32_t options);
typedef void (*SetQueueFn)(IOHIDUserDeviceRef, dispatch_queue_t);
typedef void (*SetCancelFn)(IOHIDUserDeviceRef, dispatch_block_t);
typedef void (*ActivateFn)(IOHIDUserDeviceRef);
typedef void (*CancelFn)(IOHIDUserDeviceRef);
typedef IOReturn (*ReportFn)(IOHIDUserDeviceRef, uint64_t, uint8_t *, CFIndex);
typedef IOReturn (*ReportCb)(void *refcon, int type, uint32_t reportID, uint8_t *report, CFIndex reportLength);
typedef void (*RegCbFn)(IOHIDUserDeviceRef, ReportCb, void *);

struct VPad {
    IOHIDUserDeviceRef dev;
    dispatch_queue_t queue;
    int slot;
    int identity;
    uint8_t seq;                    // DualSense 报告序号
    uint32_t ts;                    // DualSense 时间戳
    VPadRumbleFn on_rumble;
    void *rumble_ctx;
};

// ---------- dlsym 绑定 (一次性) ----------
static CreateFn pCreate;
static SetQueueFn pSetQueue;
static SetCancelFn pSetCancel;
static ActivateFn pActivate;
static CancelFn pCancel;
static ReportFn pReport;
static RegCbFn pRegSetCb, pRegGetCb;
static bool g_api_ok = false;

static void bind_api(void) {
    static bool done = false;
    if (done) return;
    done = true;
    pCreate   = (CreateFn)  dlsym(RTLD_DEFAULT, "IOHIDUserDeviceCreateWithProperties");
    pSetQueue = (SetQueueFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceSetDispatchQueue");
    pSetCancel= (SetCancelFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceSetCancelHandler");
    pActivate = (ActivateFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceActivate");
    pCancel   = (CancelFn)  dlsym(RTLD_DEFAULT, "IOHIDUserDeviceCancel");
    pReport   = (ReportFn)  dlsym(RTLD_DEFAULT, "IOHIDUserDeviceHandleReportWithTimeStamp");
    pRegSetCb = (RegCbFn)   dlsym(RTLD_DEFAULT, "IOHIDUserDeviceRegisterSetReportCallback");
    pRegGetCb = (RegCbFn)   dlsym(RTLD_DEFAULT, "IOHIDUserDeviceRegisterGetReportCallback");
    g_api_ok = pCreate && pSetQueue && pSetCancel && pActivate && pCancel && pReport;
}

// ---------- 通用手柄描述符: 16键 + Hat + 4×s16 摇杆轴 + 2×u8 扳机 + 震动输出 ----------
static const uint8_t kDescGeneric[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x85, 0x01,                                            // Report ID 1
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10,
    0x81, 0x02,                                            // 16 buttons
    0x05, 0x01, 0x09, 0x39,
    0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42,                    // hat
    0x65, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x01,        // padding
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34,
    0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F,
    0x75, 0x10, 0x95, 0x04, 0x81, 0x02,                    // X Y Rx Ry
    0x05, 0x02, 0x09, 0xC5, 0x09, 0xC4,
    0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x02,                    // Brake Accel (LT RT)
    0x85, 0x02,                                            // Report ID 2 (rumble out)
    0x06, 0x00, 0xFF, 0x09, 0x01, 0x09, 0x02,
    0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x02, 0x91, 0x02,
    0xC0
};

// ---------- CF 工具 ----------
static void cf_set_num(CFMutableDictionaryRef d, const char *k, int32_t v) {
    CFStringRef key = CFStringCreateWithCString(0, k, kCFStringEncodingUTF8);
    CFNumberRef num = CFNumberCreate(0, kCFNumberSInt32Type, &v);
    CFDictionarySetValue(d, key, num);
    CFRelease(key); CFRelease(num);
}
static void cf_set_str(CFMutableDictionaryRef d, const char *k, const char *v) {
    CFStringRef key = CFStringCreateWithCString(0, k, kCFStringEncodingUTF8);
    CFStringRef val = CFStringCreateWithCString(0, v, kCFStringEncodingUTF8);
    CFDictionarySetValue(d, key, val);
    CFRelease(key); CFRelease(val);
}

// ---------- 报告请求回调 ----------
// set-report: 游戏 → 我们。震动提取; 其余(配置/haptics)一律回成功, 否则 GCController 判定降级。
static IOReturn on_set_report(void *refcon, int type, uint32_t reportID,
                              uint8_t *report, CFIndex len) {
    VPad *p = (VPad *)refcon;
    if (type != 1 /*Output*/ || !p->on_rumble) return kIOReturnSuccess;
    if (p->identity == VHID_IDENTITY_GENERIC) {
        if (reportID == 2 && len >= 3) p->on_rumble(p->rumble_ctx, report[2], report[1]);
    } else {  // DualSense 输出报告 ID 2: [3]=右(小)马达 [4]=左(大)马达
        if (reportID == 2 && len >= 5) p->on_rumble(p->rumble_ctx, report[4], report[3]);
    }
    return kIOReturnSuccess;
}
// get-report: 罐头应答 (实测 GCController 对 DualSense 只发 output 0x02, 不发 get; 留此兜底)
static IOReturn on_get_report(void *refcon, int type, uint32_t reportID,
                              uint8_t *report, CFIndex len) {
    (void)refcon;
    if (len > 0) {
        memset(report, 0, len);
        report[0] = (uint8_t)reportID;
        if (reportID == 0x09 && len >= 7) {  // pairing info: 虚构 MAC
            report[1] = 0x00; report[2] = 0x11; report[3] = 0x22;
            report[4] = 0x33; report[5] = 0x44; report[6] = 0x55 + (uint8_t)0;
        }
    }
    return kIOReturnSuccess;
}

// ---------- 创建 ----------
VPad *vpad_create(int slot, int identity, VPadRumbleFn on_rumble, void *rumble_ctx,
                  char *err, int err_len) {
    bind_api();
    if (!g_api_ok) { snprintf(err, err_len, "IOHIDUserDevice API 符号缺失"); return NULL; }

    const uint8_t *desc; CFIndex desc_len;
    int32_t vid, pid;
    const char *mfr, *prod;
    if (identity == VHID_IDENTITY_DUALSENSE) {
        desc = kDSDesc; desc_len = sizeof(kDSDesc);
        vid = 0x054C; pid = 0x0CE6;
        mfr = "Sony Interactive Entertainment";
        prod = "DualSense Wireless Controller";
    } else {
        desc = kDescGeneric; desc_len = sizeof(kDescGeneric);
        vid = 0x1209; pid = 0x2360;
        mfr = "X360W"; prod = "X360W Controller";
    }

    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef d = CFDataCreate(0, desc, desc_len);
    CFDictionarySetValue(props, CFSTR("ReportDescriptor"), d);
    CFRelease(d);
    cf_set_num(props, "VendorID", vid);
    cf_set_num(props, "ProductID", pid);
    cf_set_num(props, "VersionNumber", 0x0100);
    cf_set_num(props, "PrimaryUsagePage", 0x01);
    cf_set_num(props, "PrimaryUsage", 0x05);
    cf_set_num(props, "LocationID", 0x360000 + slot);
    cf_set_str(props, "Manufacturer", mfr);
    char tmp[64];
    if (identity == VHID_IDENTITY_DUALSENSE) snprintf(tmp, sizeof(tmp), "%s", prod);
    else snprintf(tmp, sizeof(tmp), "%s %d", prod, slot + 1);
    cf_set_str(props, "Product", tmp);
    snprintf(tmp, sizeof(tmp), "X360W-%c%d", identity == VHID_IDENTITY_DUALSENSE ? 'D' : 'G', slot + 1);
    cf_set_str(props, "SerialNumber", tmp);
    cf_set_str(props, "Transport", "USB");

    IOHIDUserDeviceRef dev = pCreate(kCFAllocatorDefault, props, 0);
    CFRelease(props);
    if (!dev) {
        snprintf(err, err_len,
                 "IOHIDUserDevice 创建被拒 —— 需要 AMFI 放行 + 带 entitlement 签名 (见 INSTALL.md)");
        return NULL;
    }

    VPad *p = calloc(1, sizeof(VPad));
    p->dev = dev; p->slot = slot; p->identity = identity;
    p->on_rumble = on_rumble; p->rumble_ctx = rumble_ctx;
    char qname[48];
    snprintf(qname, sizeof(qname), "x360w.vhid.slot%d", slot);
    p->queue = dispatch_queue_create(qname, DISPATCH_QUEUE_SERIAL);
    pSetQueue(dev, p->queue);
    IOHIDUserDeviceRef held = dev;
    pSetCancel(dev, ^{ CFRelease(held); });
    if (pRegSetCb) pRegSetCb(dev, on_set_report, p);
    if (pRegGetCb) pRegGetCb(dev, on_get_report, p);
    pActivate(dev);
    vpad_submit_state(p, 0, 8, 0, 0, 0, 0, 0, 0);   // 中性帧激活
    return p;
}

// ---------- 语义化投递 ----------
static inline uint8_t s16_to_u8(int16_t v) { return (uint8_t)((v >> 8) + 128); }

bool vpad_submit_state(VPad *p, uint16_t b, uint8_t hat,
                       int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                       uint8_t lt, uint8_t rt) {
    if (!p || !p->dev) return false;
    IOReturn rc;
    if (p->identity == VHID_IDENTITY_DUALSENSE) {
        uint8_t r[64];
        memset(r, 0, sizeof(r));
        r[0] = 0x01;
        r[1] = s16_to_u8(lx); r[2] = s16_to_u8(ly);
        r[3] = s16_to_u8(rx); r[4] = s16_to_u8(ry);
        r[5] = lt; r[6] = rt;
        r[7] = p->seq++;
        // [8] 高半字节: □=X ✕=A ○=B △=Y
        if (b & (1 << 2)) r[8] |= 0x10;
        if (b & (1 << 0)) r[8] |= 0x20;
        if (b & (1 << 1)) r[8] |= 0x40;
        if (b & (1 << 3)) r[8] |= 0x80;
        r[8] |= hat & 0x0F;
        // [9]: L1 R1 L2btn R2btn Create Options L3 R3
        if (b & (1 << 4)) r[9] |= 0x01;
        if (b & (1 << 5)) r[9] |= 0x02;
        if (lt > 30) r[9] |= 0x04;
        if (rt > 30) r[9] |= 0x08;
        if (b & (1 << 6)) r[9] |= 0x10;
        if (b & (1 << 7)) r[9] |= 0x20;
        if (b & (1 << 8)) r[9] |= 0x40;
        if (b & (1 << 9)) r[9] |= 0x80;
        if (b & (1 << 10)) r[10] |= 0x01;   // PS
        p->ts += 1000;
        r[12] = p->ts & 0xFF; r[13] = (p->ts >> 8) & 0xFF;
        r[14] = (p->ts >> 16) & 0xFF; r[15] = (p->ts >> 24) & 0xFF;
        rc = pReport(p->dev, mach_absolute_time(), r, sizeof(r));
    } else {
        uint8_t r[VHID_INPUT_REPORT_LEN];
        memset(r, 0, sizeof(r));
        r[0] = 1;
        r[1] = b & 0xFF; r[2] = (b >> 8) & 0xFF;
        r[3] = hat & 0x0F;
        r[4] = lx & 0xFF; r[5] = (lx >> 8) & 0xFF;
        r[6] = ly & 0xFF; r[7] = (ly >> 8) & 0xFF;
        r[8] = rx & 0xFF; r[9] = (rx >> 8) & 0xFF;
        r[10] = ry & 0xFF; r[11] = (ry >> 8) & 0xFF;
        r[12] = lt; r[13] = rt;
        rc = pReport(p->dev, mach_absolute_time(), r, sizeof(r));
    }
    return rc == kIOReturnSuccess;
}

void vpad_destroy(VPad *p) {
    if (!p) return;
    vpad_submit_state(p, 0, 8, 0, 0, 0, 0, 0, 0);
    IOHIDUserDeviceRef dev = p->dev;
    p->dev = NULL;
    if (pCancel) pCancel(dev);    // cancel handler 里 CFRelease
    free(p);
}
