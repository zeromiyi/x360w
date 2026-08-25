// ds_probe.c — 虚拟 DualSense 探针: 验证 GCController/系统设置 是否接受虚拟 DualSense
// 报告格式: DualSense USB 输入报告 ID 1, 64 字节 (nondebug/dualsense 协议文档)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOReturn.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ds_desc.h"

typedef struct __IOHIDUserDevice *Dev;
typedef Dev (*CreateFn)(CFAllocatorRef, CFDictionaryRef, uint32_t);
typedef void (*SetQueueFn)(Dev, dispatch_queue_t);
typedef void (*SetCancelFn)(Dev, dispatch_block_t);
typedef void (*ActivateFn)(Dev);
typedef IOReturn (*ReportFn)(Dev, uint64_t, uint8_t *, CFIndex);

static void set_num(CFMutableDictionaryRef d, const char *k, int32_t v) {
    CFStringRef key = CFStringCreateWithCString(0, k, kCFStringEncodingUTF8);
    CFNumberRef num = CFNumberCreate(0, kCFNumberSInt32Type, &v);
    CFDictionarySetValue(d, key, num);
    CFRelease(key); CFRelease(num);
}
static void set_str(CFMutableDictionaryRef d, const char *k, const char *v) {
    CFStringRef key = CFStringCreateWithCString(0, k, kCFStringEncodingUTF8);
    CFStringRef val = CFStringCreateWithCString(0, v, kCFStringEncodingUTF8);
    CFDictionarySetValue(d, key, val);
    CFRelease(key); CFRelease(val);
}

int main(void) {
    CreateFn create = (CreateFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceCreateWithProperties");
    SetQueueFn setq = (SetQueueFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceSetDispatchQueue");
    SetCancelFn setc = (SetCancelFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceSetCancelHandler");
    ActivateFn act = (ActivateFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceActivate");
    ReportFn report = (ReportFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceHandleReportWithTimeStamp");
    if (!create || !setq || !setc || !act || !report) { printf("符号缺失\n"); return 1; }

    CFMutableDictionaryRef p = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef desc = CFDataCreate(0, kDSDesc, sizeof(kDSDesc));
    CFDictionarySetValue(p, CFSTR("ReportDescriptor"), desc); CFRelease(desc);
    set_num(p, "VendorID", 0x054C);          // Sony
    set_num(p, "ProductID", 0x0CE6);         // DualSense
    set_num(p, "VersionNumber", 0x0100);
    set_num(p, "PrimaryUsagePage", 0x01);
    set_num(p, "PrimaryUsage", 0x05);
    set_str(p, "Manufacturer", "Sony Interactive Entertainment");
    set_str(p, "Product", "DualSense Wireless Controller");
    set_str(p, "SerialNumber", "X360W-DS-1");
    set_str(p, "Transport", "USB");

    Dev dev = create(kCFAllocatorDefault, p, 0);
    CFRelease(p);
    if (!dev) { printf("创建失败 (AMFI?)\n"); return 2; }
    dispatch_queue_t q = dispatch_queue_create("ds.probe", DISPATCH_QUEUE_SERIAL);
    setq(dev, q);
    Dev held = dev;
    setc(dev, ^{ CFRelease(held); });
    act(dev);
    printf("✅ 虚拟 DualSense 已创建, 60 秒输入演示 (左摇杆画圆 + ✕ 键闪烁)\n");
    printf("请检查: 系统设置 → 游戏手柄\n");

    uint8_t r[64] = {0};
    for (int t = 0; t < 600; t++) {
        memset(r, 0, sizeof(r));
        double a = t * 0.10;
        r[0] = 0x01;
        r[1] = (uint8_t)(128 + 100 * cos(a));   // LX
        r[2] = (uint8_t)(128 + 100 * sin(a));   // LY
        r[3] = 0x80; r[4] = 0x80;               // RX RY 居中
        r[7] = (uint8_t)t;                       // seq
        r[8] = (t / 30 % 2) ? 0x20 : 0x00;       // ✕ 键闪烁 (cross)
        r[8] |= (t / 20) % 9;                    // hat 轮转
        IOReturn rc = report(dev, mach_absolute_time(), r, sizeof(r));
        if (rc != 0 && t == 0) { printf("报告投递失败 0x%08x\n", rc); return 3; }
        usleep(100000);
    }
    printf("完成。\n");
    return 0;
}
