// x360w.c — Xbox 360 无线接收器用户态驱动 (macOS 26 / Apple Silicon)
// 里程碑 M1: USB 读取链路 —— 枚举接收器、claim 每只手柄槽位接口、
//            解析存在性/输入报告、连接时点亮对应象限 LED、终端实时仪表盘。
// 协议来源: Linux xpad.c + X360MacOSReceiverBridge (研究级原型, 见 references/)
// 编译: clang -O2 -Wall -o x360w x360w.c $(pkg-config --cflags --libs libusb-1.0) -lpthread
#include <libusb-1.0/libusb.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vhid.h"

// ---------- 协议常量 ----------
static const uint16_t kVendorID = 0x045e;
static const uint16_t kKnownPIDs[] = {0x0291, 0x0719, 0x02a9}; // 原装/后期原装/山寨
#define kIfaceClass    0xff
#define kIfaceSubClass 0x5d
#define kIfaceProto    0x81   // 手柄槽位接口的 protocol 标识
#define kMaxSlots      4
#define kReadTimeoutMs 250

// 按钮位掩码 (与 X360 布局一致, 与线上位序无关)
enum {
    BTN_A = 1 << 0, BTN_B = 1 << 1, BTN_X = 1 << 2, BTN_Y = 1 << 3,
    BTN_LB = 1 << 4, BTN_RB = 1 << 5, BTN_BACK = 1 << 6, BTN_START = 1 << 7,
    BTN_L3 = 1 << 8, BTN_R3 = 1 << 9, BTN_GUIDE = 1 << 10,
};

typedef struct {
    uint16_t buttons;
    bool dpad_up, dpad_down, dpad_left, dpad_right;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
} PadState;

typedef struct {
    int slot;                       // 0..3
    int interface_number;
    uint8_t ep_in, ep_out;
    libusb_device_handle *dev;
    pthread_t thread;
    VPad *vpad;                     // 虚拟手柄 (AMFI 放行后才有)
    // 以下由读线程写、渲染线程读 (单字节/对齐读写, 竞态影响仅为一帧延迟)
    volatile bool connected;
    volatile bool headset;
    volatile unsigned long packets;
    PadState state;
} Slot;

static volatile bool g_running = true;
static Slot g_slots[kMaxSlots];
static int g_num_slots = 0;
static bool g_verbose = false;
static bool g_no_hid = false;      // --no-hid: 只跑 USB 仪表盘
static bool g_hid_broken = false;  // 创建失败过一次 (AMFI 未放行)
static int g_identity = VHID_IDENTITY_DUALSENSE;  // 默认 DualSense (GCController 可见)

static void on_sigint(int sig) { (void)sig; g_running = false; }

// ---------- 发送命令 (12 字节, 写到槽位接口的 OUT 端点) ----------
static int send_cmd(Slot *s, const uint8_t cmd[12]) {
    int transferred = 0;
    int rc = libusb_interrupt_transfer(s->dev, s->ep_out, (uint8_t *)cmd, 12,
                                       &transferred, 1000);
    if (rc != 0)
        fprintf(stderr, "[槽%d] 命令发送失败: %s\n", s->slot + 1, libusb_error_name(rc));
    return rc;
}

static void send_led(Slot *s, uint8_t mode) {  // mode: 6..9 = 槽1..4 象限常亮, 0 = 灭
    uint8_t cmd[12] = {0x00, 0x00, 0x08, (uint8_t)(0x40 + (mode & 0x0f))};
    send_cmd(s, cmd);
}

static void send_presence_query(Slot *s) {     // 主动询问当前连接状态
    uint8_t cmd[12] = {0x08, 0x00, 0x0f, 0xc0};
    send_cmd(s, cmd);
}

static void send_rumble(Slot *s, uint8_t strong, uint8_t weak) {
    uint8_t cmd[12] = {0x00, 0x01, 0x0f, 0xc0, 0x00, strong, weak};
    send_cmd(s, cmd);
}

// 震动回调: 虚拟手柄收到输出报告 → 转发给真实手柄 (refcon=Slot*)
static void vpad_rumble_bridge(void *ctx, uint8_t strong, uint8_t weak) {
    send_rumble((Slot *)ctx, strong, weak);
}

// 连接/断开时维护虚拟手柄生命周期
static void vpad_attach(Slot *s) {
    if (g_no_hid || g_hid_broken || s->vpad) return;
    char err[160] = {0};
    s->vpad = vpad_create(s->slot, g_identity, vpad_rumble_bridge, s, err, sizeof(err));
    if (!s->vpad) {
        g_hid_broken = true;   // 只提示一次, 之后保持纯 USB 模式
        fprintf(stderr, "\n*** 虚拟手柄创建失败: %s\n"
                        "*** 驱动继续以 USB 仪表盘模式运行; 放行 AMFI 后重启本程序即可获得系统级手柄\n", err);
    }
}

static void vpad_detach(Slot *s) {
    if (!s->vpad) return;
    vpad_destroy(s->vpad);
    s->vpad = NULL;
}

// ---------- 报文解析 ----------
static int16_t le_i16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// 十字键 → HID hat 值 (0=N 顺时针到 7=NW, 8=中位)
static uint8_t dpad_hat(const PadState *st) {
    int x = (st->dpad_right ? 1 : 0) - (st->dpad_left ? 1 : 0);
    int y = (st->dpad_down ? 1 : 0) - (st->dpad_up ? 1 : 0);
    if (x == 0 && y == 0) return 8;
    static const uint8_t map[3][3] = {{7, 0, 1}, {6, 8, 2}, {5, 4, 3}};
    return map[y + 1][x + 1];
}

static void handle_packet(Slot *s, const uint8_t *d, int len) {
    if (len < 2) return;

    // 状态变化包: byte0 bit3 = 存在性变化; byte1 bit7 = 手柄在场, bit6 = 耳机在场
    if (d[0] & 0x08) {
        bool present = (d[1] & 0x80) != 0;
        s->headset = (d[1] & 0x40) != 0;
        if (present != s->connected) {
            s->connected = present;
            if (present) { send_led(s, (uint8_t)(6 + s->slot)); vpad_attach(s); }
            else { vpad_detach(s); memset((void *)&s->state, 0, sizeof(PadState)); }
        }
    }

    // 输入包: byte1 == 0x01, 自 byte4 起为有线 360 格式报告
    if (d[1] == 0x01 && len >= 18 && d[4] == 0x00) {
        const uint8_t *p = d + 4;
        PadState *st = (PadState *)&s->state;
        uint8_t d0 = p[2], d1 = p[3];
        st->dpad_up = d0 & 0x01; st->dpad_down = d0 & 0x02;
        st->dpad_left = d0 & 0x04; st->dpad_right = d0 & 0x08;
        uint16_t b = 0;
        if (d0 & 0x10) b |= BTN_START; if (d0 & 0x20) b |= BTN_BACK;
        if (d0 & 0x40) b |= BTN_L3;    if (d0 & 0x80) b |= BTN_R3;
        if (d1 & 0x01) b |= BTN_LB;    if (d1 & 0x02) b |= BTN_RB;
        if (d1 & 0x04) b |= BTN_GUIDE; if (d1 & 0x10) b |= BTN_A;
        if (d1 & 0x20) b |= BTN_B;     if (d1 & 0x40) b |= BTN_X;
        if (d1 & 0x80) b |= BTN_Y;
        st->buttons = b;
        st->lt = p[4]; st->rt = p[5];
        st->lx = le_i16(p + 6); st->ly = le_i16(p + 8);
        st->rx = le_i16(p + 10); st->ry = le_i16(p + 12);
        if (!s->connected) {          // 有输入即证明在场 (补错过的状态包)
            s->connected = true;
            send_led(s, (uint8_t)(6 + s->slot));
            vpad_attach(s);
        }
        // 投递虚拟 HID 报告: Y 轴取反(HID 向下为正), 十字键同时映射为按钮 12-15
        if (s->vpad) {
            uint16_t b = st->buttons;
            if (st->dpad_up) b |= 1 << 11;
            if (st->dpad_down) b |= 1 << 12;
            if (st->dpad_left) b |= 1 << 13;
            if (st->dpad_right) b |= 1 << 14;
            vpad_submit_state(s->vpad, b, dpad_hat(st),
                              st->lx, (int16_t)~st->ly, st->rx, (int16_t)~st->ry,
                              st->lt, st->rt);
        }
    }

    if (g_verbose) {
        printf("[槽%d] %2d字节:", s->slot + 1, len);
        for (int i = 0; i < len; i++) printf(" %02x", d[i]);
        printf("\n");
    }
}

// ---------- 读线程 ----------
static void *reader_main(void *arg) {
    Slot *s = (Slot *)arg;
    uint8_t buf[64];
    while (g_running) {
        int transferred = 0;
        int rc = libusb_interrupt_transfer(s->dev, s->ep_in, buf, sizeof(buf),
                                           &transferred, kReadTimeoutMs);
        if (rc == LIBUSB_ERROR_TIMEOUT) continue;
        if (rc != 0) {
            if (g_running)
                fprintf(stderr, "[槽%d] 读取错误: %s (接收器被拔出?)\n",
                        s->slot + 1, libusb_error_name(rc));
            break;
        }
        s->packets++;
        handle_packet(s, buf, transferred);
    }
    return NULL;
}

// ---------- 仪表盘 ----------
static const char *hat_str(const PadState *st) {
    if (st->dpad_up && st->dpad_right) return "↗";
    if (st->dpad_up && st->dpad_left) return "↖";
    if (st->dpad_down && st->dpad_right) return "↘";
    if (st->dpad_down && st->dpad_left) return "↙";
    if (st->dpad_up) return "↑"; if (st->dpad_down) return "↓";
    if (st->dpad_left) return "←"; if (st->dpad_right) return "→";
    return "·";
}

static void render(void) {
    printf("\033[H\033[J");  // 清屏并归位
    printf("x360w · Xbox 360 无线接收器用户态驱动 (M1: USB 链路验证)\n");
    printf("======================================================\n");
    for (int i = 0; i < g_num_slots; i++) {
        Slot *s = &g_slots[i];
        if (!s->connected) {
            printf("[槽%d] ○ 未连接  (按手柄顶部 Connect 键配对, 已收包 %lu)\n",
                   i + 1, s->packets);
            continue;
        }
        PadState *st = (PadState *)&s->state;
        char btn[64] = {0};
        strcat(btn, (st->buttons & BTN_A) ? "A " : "- ");
        strcat(btn, (st->buttons & BTN_B) ? "B " : "- ");
        strcat(btn, (st->buttons & BTN_X) ? "X " : "- ");
        strcat(btn, (st->buttons & BTN_Y) ? "Y " : "- ");
        strcat(btn, (st->buttons & BTN_LB) ? "LB " : "-- ");
        strcat(btn, (st->buttons & BTN_RB) ? "RB " : "-- ");
        strcat(btn, (st->buttons & BTN_BACK) ? "BK " : "-- ");
        strcat(btn, (st->buttons & BTN_START) ? "ST " : "-- ");
        strcat(btn, (st->buttons & BTN_L3) ? "L3 " : "-- ");
        strcat(btn, (st->buttons & BTN_R3) ? "R3 " : "-- ");
        strcat(btn, (st->buttons & BTN_GUIDE) ? "◉" : "- ");
        printf("[槽%d] ● 已连接%s%s\n", i + 1, s->headset ? " +耳机" : "",
               s->vpad ? " 🎮虚拟手柄在线" : "");
        printf("  按钮: %s  十字: %s\n", btn, hat_str(st));
        printf("  扳机: LT=%3d RT=%3d   左摇杆: %6d,%6d   右摇杆: %6d,%6d\n",
               st->lt, st->rt, st->lx, st->ly, st->rx, st->ry);
    }
    printf("======================================================\n");
    printf("Ctrl+C 退出%s\n", g_verbose ? " | --verbose 已开启" : "");
    fflush(stdout);
}

// ---------- 设备发现 ----------
static bool is_known_pid(uint16_t pid) {
    for (size_t i = 0; i < sizeof(kKnownPIDs) / sizeof(kKnownPIDs[0]); i++)
        if (kKnownPIDs[i] == pid) return true;
    return false;
}

int main(int argc, char **argv) {
    uint16_t want_vid = kVendorID, want_pid = 0;  // pid=0 → 匹配任一已知
    bool hid_probe = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) g_verbose = true;
        else if (!strcmp(argv[i], "--no-hid")) g_no_hid = true;
        else if (!strcmp(argv[i], "--hid-probe")) hid_probe = true;
        else if (!strcmp(argv[i], "--identity") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "generic") || !strcmp(v, "g")) g_identity = VHID_IDENTITY_GENERIC;
            else g_identity = VHID_IDENTITY_DUALSENSE;
        }
        else if (!strcmp(argv[i], "--pid") && i + 1 < argc) want_pid = (uint16_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--vid") && i + 1 < argc) want_vid = (uint16_t)strtol(argv[++i], NULL, 0);
        else {
            printf("用法: %s [--vid 0x045e] [--pid 0x0291] [--no-hid] [--hid-probe] [-v]\n", argv[0]);
            printf("  --no-hid     只跑 USB 仪表盘, 不建虚拟手柄\n");
            printf("  --hid-probe  独立虚拟手柄探针 (不需要接收器, 验证 AMFI 放行是否生效)\n");
            return 0;
        }
    }

    // 探针模式: 创建一个虚拟手柄并摇摆左摇杆 30 秒, 供系统设置/gamepad-tester 观察
    if (hid_probe) {
        char err[160] = {0};
        printf("[hid-probe] 创建虚拟手柄 (%s)…\n",
               g_identity == VHID_IDENTITY_DUALSENSE ? "DualSense" : "Generic");
        VPad *p = vpad_create(0, g_identity, NULL, NULL, err, sizeof(err));
        if (!p) { printf("[hid-probe] 失败: %s\n", err); return 2; }
        printf("[hid-probe] ✅ 创建成功! 30 秒演示 —— 请检查 系统设置 → 游戏手柄\n");
        for (int t = 0; t < 300; t++) {
            double a = t * 0.21;
            vpad_submit_state(p, (t / 25 % 2) ? BTN_A : 0, (uint8_t)(t / 12 % 9),
                              (int16_t)(30000 * cos(a)), (int16_t)(30000 * sin(a)),
                              0, 0, (uint8_t)(t % 256), 0);
            usleep(100000);
        }
        vpad_destroy(p);
        printf("[hid-probe] 完成。\n");
        return 0;
    }

    int rc = libusb_init(NULL);
    if (rc != 0) { fprintf(stderr, "libusb_init 失败: %s\n", libusb_error_name(rc)); return 1; }

    // 找接收器
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(NULL, &list);
    libusb_device *found = NULL;
    struct libusb_device_descriptor desc;
    for (ssize_t i = 0; i < n; i++) {
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        if (desc.idVendor != want_vid) continue;
        if (want_pid ? desc.idProduct == want_pid : is_known_pid(desc.idProduct)) {
            found = list[i];
            break;
        }
    }
    if (!found) {
        fprintf(stderr, "未找到 Xbox 360 无线接收器 (VID %04x)。\n"
                        "请确认: ①接收器已插入(可经 Hub) ②system_profiler SPUSBDataType 能看到它\n", want_vid);
        libusb_free_device_list(list, 1);
        libusb_exit(NULL);
        return 2;
    }
    printf("找到接收器: %04x:%04x (bus %d addr %d)%s\n", desc.idVendor, desc.idProduct,
           libusb_get_bus_number(found), libusb_get_device_address(found),
           is_known_pid(desc.idProduct) ? "" : " [自定义 PID]");

    libusb_device_handle *dev = NULL;
    rc = libusb_open(found, &dev);
    if (rc != 0) {
        fprintf(stderr, "打开失败: %s\n", libusb_error_name(rc));
        libusb_free_device_list(list, 1);
        libusb_exit(NULL);
        return 3;
    }

    // 枚举手柄槽位接口 (FF/5D/81), 每槽一组中断 IN/OUT 端点
    struct libusb_config_descriptor *cfg = NULL;
    rc = libusb_get_active_config_descriptor(found, &cfg);
    if (rc != 0) rc = libusb_get_config_descriptor(found, 0, &cfg);
    if (rc != 0 || !cfg) { fprintf(stderr, "读取配置描述符失败\n"); return 4; }

    for (uint8_t i = 0; i < cfg->bNumInterfaces && g_num_slots < kMaxSlots; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
            if (alt->bInterfaceClass != kIfaceClass ||
                alt->bInterfaceSubClass != kIfaceSubClass ||
                alt->bInterfaceProtocol != kIfaceProto) continue;
            Slot *s = &g_slots[g_num_slots];
            memset(s, 0, sizeof(*s));
            s->slot = g_num_slots;
            s->interface_number = alt->bInterfaceNumber;
            for (uint8_t e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_INTERRUPT) continue;
                if (ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) s->ep_in = ep->bEndpointAddress;
                else s->ep_out = ep->bEndpointAddress;
            }
            if (!s->ep_in || !s->ep_out) continue;
            g_num_slots++;
            break;  // 每接口取一个 altsetting 即可
        }
    }
    libusb_free_config_descriptor(cfg);
    printf("发现 %d 个手柄槽位接口\n", g_num_slots);
    if (g_num_slots == 0) {
        fprintf(stderr, "没有找到 FF/5D/81 接口——山寨接收器协议可能不同, 用 -v 跑 system_profiler 发我描述符\n");
        return 5;
    }

    // claim 接口 + 启动读线程
    for (int i = 0; i < g_num_slots; i++) {
        Slot *s = &g_slots[i];
        s->dev = dev;
        rc = libusb_claim_interface(dev, s->interface_number);
        if (rc != 0) {
            fprintf(stderr, "[槽%d] claim 接口 %d 失败: %s (被其他驱动占用?)\n",
                    i + 1, s->interface_number, libusb_error_name(rc));
            return 6;
        }
        send_presence_query(s);   // 让接收器上报当前状态
        pthread_create(&s->thread, NULL, reader_main, s);
    }
    printf("已 claim 全部槽位, 开始监听。按 Ctrl+C 退出。\n");
    sleep(1);

    signal(SIGINT, on_sigint);
    while (g_running) {
        if (isatty(fileno(stdout))) { render(); usleep(66000); }  // 终端前才画仪表盘
        else usleep(500000);                                      // 后台/launchd 模式静默
    }

    // 收尾: 灭灯 + 销毁虚拟手柄 + 释放
    printf("\n退出中: 熄灭 LED 并释放接口…\n");
    for (int i = 0; i < g_num_slots; i++) { vpad_detach(&g_slots[i]); send_led(&g_slots[i], 0); }
    for (int i = 0; i < g_num_slots; i++) pthread_join(g_slots[i].thread, NULL);
    for (int i = 0; i < g_num_slots; i++) libusb_release_interface(dev, g_slots[i].interface_number);
    libusb_close(dev);
    libusb_free_device_list(list, 1);
    libusb_exit(NULL);
    printf("完成。\n");
    return 0;
}
