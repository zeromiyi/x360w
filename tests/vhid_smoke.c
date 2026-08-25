// vhid_smoke.c — IOHIDUserDevice 无授权可行性冒烟测试 (macOS 26 / arm64)
// 目的: 不签名/无 com.apple.developer.hid.virtual.device 的情况下,
//       能否创建虚拟 HID 游戏手柄并投递报告。
// 用法: ./vhid_smoke  (驻留 90 秒,期间用 ioreg / 系统设置 / gamepad-tester 观察)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>

typedef struct __IOHIDUserDevice *IOHIDUserDeviceRef;
typedef IOHIDUserDeviceRef (*CreateFn)(CFAllocatorRef, CFDictionaryRef, IOOptionBits);
typedef void (*SetQueueFn)(IOHIDUserDeviceRef, dispatch_queue_t);
typedef void (*SetCancelFn)(IOHIDUserDeviceRef, dispatch_block_t);
typedef void (*ActivateFn)(IOHIDUserDeviceRef);
typedef IOReturn (*ReportFn)(IOHIDUserDeviceRef, uint64_t, uint8_t *, CFIndex);

// 最小 Gamepad 描述符: 8按钮 + Hat + X/Y 16位摇杆 → 6 字节报告
static const uint8_t kDesc[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Min (1)
    0x29, 0x08,       //   Usage Max (8)
    0x15, 0x00,       //   Logical Min (0)
    0x25, 0x01,       //   Logical Max (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x08,       //   Report Count (8)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x05, 0x01,       //   Usage Page (Generic Desktop)
    0x09, 0x39,       //   Usage (Hat switch)
    0x15, 0x00, 0x25, 0x07,
    0x35, 0x00, 0x46, 0x3B, 0x01,
    0x65, 0x14,       //   Unit (Degrees)
    0x75, 0x04, 0x95, 0x01,
    0x81, 0x42,       //   Input (Data,Var,Abs,NullState)
    0x75, 0x04, 0x95, 0x01,
    0x81, 0x03,       //   4-bit padding (Const)
    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x16, 0x01, 0x80, //   Logical Min (-32767)
    0x26, 0xFF, 0x7F, //   Logical Max (32767)
    0x75, 0x10, 0x95, 0x02,
    0x81, 0x02,
    0xC0
};

static void set_num(CFMutableDictionaryRef d, const char *k, int32_t v) {
    CFStringRef key = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFNumberRef num = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    CFDictionarySetValue(d, key, num);
    CFRelease(key); CFRelease(num);
}
static void set_str(CFMutableDictionaryRef d, const char *k, const char *v) {
    CFStringRef key = CFStringCreateWithCString(NULL, k, kCFStringEncodingUTF8);
    CFStringRef val = CFStringCreateWithCString(NULL, v, kCFStringEncodingUTF8);
    CFDictionarySetValue(d, key, val);
    CFRelease(key); CFRelease(val);
}

int main(void) {
    CreateFn create = (CreateFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceCreateWithProperties");
    SetQueueFn setq = (SetQueueFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceSetDispatchQueue");
    SetCancelFn setc = (SetCancelFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceSetCancelHandler");
    ActivateFn act = (ActivateFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceActivate");
    ReportFn report = (ReportFn)dlsym(RTLD_DEFAULT, "IOHIDUserDeviceHandleReportWithTimeStamp");
    printf("[1] dlsym: create=%p setq=%p setc=%p act=%p report=%p\n",
           (void*)create, (void*)setq, (void*)setc, (void*)act, (void*)report);
    if (!create || !report) { printf("FATAL: 符号缺失\n"); return 1; }

    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef descData = CFDataCreate(NULL, kDesc, sizeof(kDesc));
    CFDictionarySetValue(props, CFSTR("ReportDescriptor"), descData);
    CFRelease(descData);
    set_num(props, "VendorID", 0x1209);      // pid.codes 社区 VID, 不冒充 Xbox
    set_num(props, "ProductID", 0x2360);
    set_num(props, "VersionNumber", 0x0100);
    set_num(props, "PrimaryUsagePage", 0x01);
    set_num(props, "PrimaryUsage", 0x05);    // Game Pad
    set_str(props, "Manufacturer", "X360W-Userspace");
    set_str(props, "Product", "X360W Smoke Test Pad");
    set_str(props, "SerialNumber", "X360W-SMOKE-1");
    set_str(props, "Transport", "USB");

    IOHIDUserDeviceRef dev = create(kCFAllocatorDefault, props, 0);
    CFRelease(props);
    printf("[2] IOHIDUserDeviceCreateWithProperties → %p\n", (void*)dev);
    if (!dev) {
        printf("RESULT=CREATE_FAILED  (无授权时被拦截 → 需要 managed entitlement)\n");
        return 2;
    }

    if (setq && setc && act) {
        dispatch_queue_t q = dispatch_queue_create("vhid.smoke", DISPATCH_QUEUE_SERIAL);
        setq(dev, q);
        setc(dev, ^{ CFRelease(dev); });
        act(dev);
        printf("[3] activate 完成\n");
    }

    // 中性报告: 按钮0, hat=8(中位), X=Y=0
    uint8_t neutral[6] = {0, 8, 0, 0, 0, 0};
    IOReturn r = report(dev, mach_absolute_time(), neutral, sizeof(neutral));
    printf("[4] HandleReportWithTimeStamp → 0x%08x (%s)\n", r, r == 0 ? "kIOReturnSuccess" : "ERR");
    printf("RESULT=CREATE_OK — 驻留 90 秒, 请观察:\n");
    printf("  · ioreg -rc IOHIDUserDevice\n");
    printf("  · 系统设置 → 游戏手柄\n");
    printf("  · https://gamepad-tester.com\n");
    fflush(stdout);
    for (int i = 0; i < 90; i++) sleep(1);
    return 0;
}
