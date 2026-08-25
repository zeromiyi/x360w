// rumble_test.c — 全链路震动测试: 找到虚拟手柄 → 发 HID 输出报告(ID 2)
// → 驱动的 set-report 回调 → 接收器震动命令 → 真实手柄震动
#include <IOKit/hid/IOHIDManager.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(
        0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int32_t vid = 0x1209, pid = 0x2360;
    CFNumberRef n = CFNumberCreate(0, kCFNumberSInt32Type, &vid);
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), n); CFRelease(n);
    n = CFNumberCreate(0, kCFNumberSInt32Type, &pid);
    CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), n); CFRelease(n);
    IOHIDManagerSetDeviceMatching(mgr, match);
    CFRelease(match);
    if (IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        printf("IOHIDManager 打开失败\n"); return 1;
    }
    CFSetRef devs = IOHIDManagerCopyDevices(mgr);
    if (!devs || CFSetGetCount(devs) == 0) {
        printf("❌ 找不到 X360W 虚拟手柄 (驱动 ./x360w-signed 在跑吗? 手柄配对了吗?)\n"); return 2;
    }
    CFIndex count = CFSetGetCount(devs);
    const void **values = (const void **)malloc(count * sizeof(void *));
    CFSetGetValues(devs, values);
    IOHIDDeviceRef dev = (IOHIDDeviceRef)values[0];
    printf("找到虚拟手柄, 3 段震动: 强 → 弱 → 双马达\n");
    uint8_t seq[3][3] = {{2, 255, 0}, {2, 0, 255}, {2, 200, 200}};
    const char *names[3] = {"强马达", "弱马达", "双马达"};
    for (int i = 0; i < 3; i++) {
        printf("  [%d] %s …\n", i + 1, names[i]);
        IOReturn r = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 2, seq[i], 3);
        printf("      SetReport → 0x%08x %s\n", r, r == kIOReturnSuccess ? "✅" : "❌");
        usleep(900000);
        uint8_t stop[3] = {2, 0, 0};
        IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 2, stop, 3);
        usleep(400000);
    }
    printf("完成 —— 手柄震了吗?\n");
    return 0;
}
