// ds_probe2.c — 虚拟 DualSense v2: 记录并应答 GCController 的 feature/output 报告请求
// 目的: 找出"未连接"状态缺哪次握手; 应答后看是否转为已连接
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
typedef IOReturn (*ReportCb)(void *, int, uint32_t, uint8_t *, CFIndex);
typedef void (*RegCbFn)(Dev, ReportCb, void *);

// GET 请求: 记录 + 返回罐头数据 (内容先全零, report[0]=ID)
static IOReturn on_get(void *ctx, int type, uint32_t id, uint8_t *rep, CFIndex len) {
    fprintf(stderr, "[GET ] type=%d id=0x%02x bufLen=%ld\n", type, id, (long)len);
    memset(rep, 0, len);
    rep[0] = (uint8_t)id;
    if (id == 0x09 && len >= 7) {   // pairing info: 塞个 MAC
        rep[1] = 0x00; rep[2] = 0x11; rep[3] = 0x22;
        rep[4] = 0x33; rep[5] = 0x44; rep[6] = 0x55;
    }
    if (id == 0x20 && len >= 4) {   // firmware info: 塞个版本号区
        rep[1] = 0x30; rep[2] = 0x30; rep[3] = 0x30;
    }
    return kIOReturnSuccess;
}
// SET 请求: 记录 + 全收 (haptics 配置/震动都回成功)
static IOReturn on_set(void *ctx, int type, uint32_t id, uint8_t *rep, CFIndex len) {
    fprintf(stderr, "[SET ] type=%d id=0x%02x len=%ld data=[%02x %02x %02x %02x %02x]\n",
            type, id, (long)len,
            len > 1 ? rep[1] : 0, len > 2 ? rep[2] : 0, len > 3 ? rep[3] : 0,
            len > 4 ? rep[4] : 0, len > 5 ? rep[5] : 0);
    return kIOReturnSuccess;
}

int main(void) {
    CreateFn create = (CreateFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceCreateWithProperties");
    SetQueueFn setq = (SetQueueFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceSetDispatchQueue");
    SetCancelFn setc = (SetCancelFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceSetCancelHandler");
    ActivateFn act = (ActivateFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceActivate");
    ReportFn report = (ReportFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceHandleReportWithTimeStamp");
    RegCbFn regGet = (RegCbFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceRegisterGetReportCallback");
    RegCbFn regSet = (RegCbFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceRegisterSetReportCallback");

    CFMutableDictionaryRef p = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef desc = CFDataCreate(0, kDSDesc, sizeof(kDSDesc));
    CFDictionarySetValue(p, CFSTR("ReportDescriptor"), desc); CFRelease(desc);
    int32_t n;
    #define NUM(k, v) n = v; { CFStringRef k_ = CFSTR(k); CFNumberRef n_ = CFNumberCreate(0, kCFNumberSInt32Type, &n); CFDictionarySetValue(p, k_, n_); CFRelease(k_); CFRelease(n_); }
    NUM("VendorID", 0x054C); NUM("ProductID", 0x0CE6); NUM("VersionNumber", 0x0100);
    NUM("PrimaryUsagePage", 0x01); NUM("PrimaryUsage", 0x05);
    CFStringRef ks, vs;
    #define STR(k, v) ks = CFSTR(k); vs = CFSTR(v); CFDictionarySetValue(p, ks, vs);
    STR("Manufacturer", "Sony Interactive Entertainment");
    STR("Product", "DualSense Wireless Controller");
    STR("SerialNumber", "X360W-DS-1");
    STR("Transport", "USB");

    Dev dev = create(kCFAllocatorDefault, p, 0);
    CFRelease(p);
    if (!dev) { printf("创建失败\n"); return 2; }
    dispatch_queue_t q = dispatch_queue_create("ds.probe2", DISPATCH_QUEUE_SERIAL);
    setq(dev, q);
    Dev held = dev;
    setc(dev, ^{ CFRelease(held); });
    if (regGet) regGet(dev, on_get, NULL); else fprintf(stderr, "!! 无 get 回调符号\n");
    if (regSet) regSet(dev, on_set, NULL); else fprintf(stderr, "!! 无 set 回调符号\n");
    act(dev);
    printf("✅ 探针 v2 运行中 (90 秒)。请打开 系统设置 → 游戏手柄 观察状态变化\n");
    fprintf(stderr, "—— 以下为系统发来的报告请求日志 ——\n");

    uint8_t r[64];
    for (int t = 0; t < 900; t++) {
        memset(r, 0, sizeof(r));
        double a = t * 0.10;
        r[0] = 0x01;
        r[1] = (uint8_t)(128 + 100 * cos(a));
        r[2] = (uint8_t)(128 + 100 * sin(a));
        r[3] = 0x80; r[4] = 0x80;
        r[7] = (uint8_t)t;
        r[8] = (t / 30 % 2) ? 0x20 : 0x00;
        r[8] |= (t / 20) % 9;
        report(dev, mach_absolute_time(), r, sizeof(r));
        usleep(100000);
    }
    printf("完成。\n");
    return 0;
}
