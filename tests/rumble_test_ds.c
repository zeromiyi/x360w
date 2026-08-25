// rumble_test_ds.c — 全链路震动测试 (DualSense 身份版):
// 找到虚拟 DualSense → 发 DS 输出报告(ID 2, [3]=小马达 [4]=大马达)
// → 驱动的 set-report 回调 → 接收器震动命令 → 真实手柄震动
#include <IOKit/hid/IOHIDManager.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(
        0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int32_t vid = 0x054C, pid = 0x0CE6;
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
        printf("❌ 找不到虚拟 DualSense (驱动在跑吗? 手柄配对了吗?)\n"); return 2;
    }
    CFIndex count = CFSetGetCount(devs);
    const void **values = (const void **)malloc(count * sizeof(void *));
    CFSetGetValues(devs, values);
    IOHIDDeviceRef dev = (IOHIDDeviceRef)values[0];
    printf("找到虚拟 DualSense, 3 段震动: 强 → 弱 → 双马达\n");
    // DS 输出报告: [0]=0x02 [1]=valid_flag0 [2]=valid_flag1 [3]=小马达 [4]=大马达, 共 48 字节
    uint8_t r[48];
    uint8_t seq[3][2] = {{0, 255}, {255, 0}, {200, 200}};
    const char *names[3] = {"强马达(左)", "弱马达(右)", "双马达"};
    for (int i = 0; i < 3; i++) {
        memset(r, 0, sizeof(r));
        r[0] = 0x02; r[1] = 0xFF; r[2] = 0xF7;   // flags: 全开 (与 GCController 配置同款)
        r[3] = seq[i][0]; r[4] = seq[i][1];
        printf("  [%d] %s …\n", i + 1, names[i]);
        IOReturn rc = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 2, r, sizeof(r));
        printf("      SetReport → 0x%08x %s\n", rc, rc == kIOReturnSuccess ? "✅" : "❌");
        usleep(900000);
        r[3] = 0; r[4] = 0;
        IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 2, r, sizeof(r));
        usleep(400000);
    }
    printf("完成 —— 手柄震了吗?\n");
    return 0;
}
