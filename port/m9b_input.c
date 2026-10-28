// Descent on the m9b SoC: start-up, timer, keyboard, mouse and (absent)
// joystick, in place of D1X's arch/sdl and arch/linux.
//
// Keys: USB keyboards (../../core/rtl/usb_port.v, polled) report HID usage
// codes, which are the ARCH_KEY_* codes (port/key_arch.h) main/key.c works
// with; their presses and releases go to generic_key_handler(). Keys from
// the UART (run.py's terminal, tools/d1.py): arrows, Enter, Esc, printable
// characters; a terminal only sends presses, so a UART key counts as held
// until 150 ms after its last repeat. The UART also takes tools/d1.py's
// screenshot request (0x1C, see m9b_video.c).
//
// Mouse: motion from the hardware's running sums of every report, buttons
// from the last report; the functions are D1X's SDL mouse driver's.
#include <stdint.h>
#include <string.h>

#include "clock.h"   // CLOCK_HZ: ../../core/firmware
#include "fix.h"
#include "joy.h"
#include "key.h"
#include "key_arch.h"
#include "mouse.h"
#include "timer.h"
#include "m9b_map.h"

#define CLOCK_CNT     (*(volatile uint32_t*)(M9B_TIMING + 0xCu))
#define UART_DATA     (*(volatile uint32_t*)M9B_UART)
#define UART_RX_VALID (1u << 8)

extern void generic_key_handler(int event_key, int key_state);   // main/key.c
extern void m9b_screenshot(void);                                // m9b_video.c
extern void m9b_digi_poll(void);                                 // m9b_digi.c

// -------------------------------------------------------------------------
// Timer: fix seconds since start-up (16.16), from the 36 MHz CLOCK_CNT.
// -------------------------------------------------------------------------
static uint32_t ms_now(void) {
    static uint32_t last, ms, frac;
    uint32_t now = CLOCK_CNT;
    frac += now - last;
    last = now;
    ms += frac / CLOCK_PER_MS;
    frac %= CLOCK_PER_MS;
    return ms;
}

fix timer_get_fixed_seconds(void) {
    // ms * 65536 / 1000, without overflowing for the first 9 hours.
    uint32_t ms = ms_now();
    return (fix)((uint64_t)ms * 65536u / 1000u);
}

void timer_init(void) {}

// -------------------------------------------------------------------------
// Mouse (D1X's arch/sdl/mouse.c, fed from usb_poll below)
// -------------------------------------------------------------------------
struct mousebutton {
    ubyte pressed;
    fix time_went_down, time_held_down;
    uint num_downs, num_ups;
};
static struct {
    struct mousebutton buttons[MOUSE_MAX_BUTTONS];
    int min_x, min_y, max_x, max_y;
    int delta_x, delta_y, x, y;
} Mouse;

void event_poll(void);

static void mouse_button(int button, int down) {
    struct mousebutton *b = &Mouse.buttons[button];
    if (down) {
        b->pressed = 1;
        b->time_went_down = timer_get_fixed_seconds();
        b->num_downs++;
    } else {
        b->pressed = 0;
        b->time_held_down += timer_get_fixed_seconds() - b->time_went_down;
        b->num_ups++;
    }
}

static void mouse_motion(int dx, int dy) {
    Mouse.delta_x += dx;
    Mouse.delta_y += dy;
    Mouse.x += dx;
    Mouse.y += dy;
    if (Mouse.x > Mouse.max_x) Mouse.x = Mouse.max_x;
    else if (Mouse.x < Mouse.min_x) Mouse.x = Mouse.min_x;
    if (Mouse.y > Mouse.max_y) Mouse.y = Mouse.max_y;
    else if (Mouse.y < Mouse.min_y) Mouse.y = Mouse.min_y;
}

int mouse_set_limits(int x1, int y1, int x2, int y2) {
    Mouse.min_x = x1; Mouse.min_y = y1;
    Mouse.max_x = x2; Mouse.max_y = y2;
    return 0;
}

void mouse_flush(void) {
    event_poll();
    fix now = timer_get_fixed_seconds();
    for (int i = 0; i < MOUSE_MAX_BUTTONS; i++) {
        Mouse.buttons[i].pressed = 0;
        Mouse.buttons[i].time_went_down = now;
        Mouse.buttons[i].time_held_down = 0;
        Mouse.buttons[i].num_ups = Mouse.buttons[i].num_downs = 0;
    }
    Mouse.delta_x = Mouse.delta_y = 0;
    Mouse.x = Mouse.y = 0;
}

void mouse_get_pos(int *x, int *y) { event_poll(); *x = Mouse.x; *y = Mouse.y; }

void mouse_get_delta(int *dx, int *dy) {
    event_poll();
    *dx = Mouse.delta_x;
    *dy = Mouse.delta_y;
    Mouse.delta_x = Mouse.delta_y = 0;
}

int mouse_get_btns(void) {
    int status = 0;
    event_poll();
    for (int i = 0; i < MOUSE_MAX_BUTTONS; i++)
        if (Mouse.buttons[i].pressed) status |= 1 << i;
    return status;
}

void mouse_set_pos(int x, int y) {
    event_poll();
    Mouse.x = x;
    Mouse.y = y;
    mouse_motion(0, 0);
}

void mouse_get_cyberman_pos(int *x, int *y) { (void)x; (void)y; event_poll(); }

fix mouse_button_down_time(int button) {
    fix t;
    event_poll();
    if (!Mouse.buttons[button].pressed) {
        t = Mouse.buttons[button].time_held_down;
        Mouse.buttons[button].time_held_down = 0;
    } else {
        fix now = timer_get_fixed_seconds();
        t = now - Mouse.buttons[button].time_held_down;
        Mouse.buttons[button].time_held_down = now;
    }
    return t;
}

int mouse_button_down_count(int button) {
    event_poll();
    int n = Mouse.buttons[button].num_downs;
    Mouse.buttons[button].num_downs = 0;
    return n;
}

int mouse_button_state(int button) {
    event_poll();
    return Mouse.buttons[button].pressed;
}

// -------------------------------------------------------------------------
// UART keys
// -------------------------------------------------------------------------
#define HOLD_MS 150u
static struct { int key; uint32_t until; } held[8];
static uint32_t esc_state, esc_time;   // 0, 1 after ESC, 2 after ESC [

static void uart_key(int key, int shift) {
    uint32_t now = ms_now();
    for (int i = 0; i < 8; i++)
        if (held[i].key == key) { held[i].until = now + HOLD_MS; return; }
    for (int i = 0; i < 8; i++)
        if (!held[i].key) {
            held[i].key = key;
            held[i].until = now + HOLD_MS;
            if (shift) generic_key_handler(ARCH_KEY_LSHIFT, 1);
            generic_key_handler(key, 1);
            if (shift) generic_key_handler(ARCH_KEY_LSHIFT, 0);
            return;
        }
}

// A character's HID key (and whether it needs shift).
static int hid_of_char(unsigned char c, int *shift) {
    static const char plain[] = "\0\0\0\0abcdefghijklmnopqrstuvwxyz1234567890\0\0\0\0 -=[]\\\0;'`,./";
    static const char shifted[] = "\0\0\0\0ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\0\0\0\0 _+{}|\0:\"~<>?";
    *shift = 0;
    for (int k = 4; k < (int)sizeof(plain) - 1; k++) {
        if (plain[k] && plain[k] == (char)c) return k;
        if (shifted[k] && shifted[k] == (char)c) { *shift = 1; return k; }
    }
    return 0;
}

static void poll_uart(void) {
    uint32_t now = ms_now();
    if (esc_state && now - esc_time > 30u) { esc_state = 0; uart_key(ARCH_KEY_ESC, 0); }
    for (;;) {
        uint32_t d = UART_DATA;
        if (!(d & UART_RX_VALID)) break;
        unsigned char c = (unsigned char)d;
        if (esc_state == 1) {
            esc_state = c == '[' ? 2u : 0u;
            if (!esc_state) uart_key(ARCH_KEY_ESC, 0);
            continue;
        }
        if (esc_state == 2) {
            esc_state = 0;
            if (c == 'A') uart_key(ARCH_KEY_UP, 0);
            else if (c == 'B') uart_key(ARCH_KEY_DOWN, 0);
            else if (c == 'C') uart_key(ARCH_KEY_RIGHT, 0);
            else if (c == 'D') uart_key(ARCH_KEY_LEFT, 0);
            continue;
        }
        if (c == 27) { esc_state = 1; esc_time = now; }
        else if (c == 0x1Cu) m9b_screenshot();
        else if (c == '\r' || c == '\n') uart_key(ARCH_KEY_ENTER, 0);
        else if (c == 8 || c == 127) uart_key(ARCH_KEY_BACKSP, 0);
        else if (c >= 32 && c < 127) {
            int shift, k = hid_of_char(c, &shift);
            if (k) uart_key(k, shift);
        }
    }
    for (int i = 0; i < 8; i++)
        if (held[i].key && (int32_t)(now - held[i].until) >= 0) {
            generic_key_handler(held[i].key, 0);
            held[i].key = 0;
        }
}

// -------------------------------------------------------------------------
// USB keyboards and mice (see ../../doom/port/doomgeneric_m9b.c)
// -------------------------------------------------------------------------
#define USB_REG(p, o)    (*(volatile uint32_t*)(M9B_USB + 0x20u * (p) + (o)))
#define USB_STATUS(p)    USB_REG(p, 0x00u)   // bits 1:0 type, 15:8 report count
#define USB_KEYS(p)      USB_REG(p, 0x04u)   // {k3, k2, k1, modifiers}
#define USB_KEYS2(p)     USB_REG(p, 0x08u)   // k4
#define USB_MOUSE(p)     USB_REG(p, 0x0Cu)   // {dy, dx, buttons}
#define USB_MOUSE_POS(p) USB_REG(p, 0x14u)   // {y, x}, 16-bit running sums

static struct {
    uint32_t type, count;
    uint8_t keys[4], mods, buttons;
    uint32_t pos;
} usb[2];

static int key_in(uint8_t k, const uint8_t *keys) {
    for (int i = 0; i < 4; i++)
        if (keys[i] == k) return 1;
    return 0;
}

static void usb_keys_changed(const uint8_t *old_keys, uint8_t old_mods, const uint8_t *keys, uint8_t mods) {
    for (int b = 0; b < 8; b++)   // modifier bits = HID usages 0xE0..0xE7
        if (((old_mods ^ mods) >> b) & 1u) generic_key_handler(0xE0 + b, (mods >> b) & 1);
    for (int i = 0; i < 4; i++)
        if (old_keys[i] > 3u && !key_in(old_keys[i], keys)) generic_key_handler(old_keys[i], 0);
    for (int i = 0; i < 4; i++)
        if (keys[i] > 3u && !key_in(keys[i], old_keys)) generic_key_handler(keys[i], 1);
}

static void usb_poll(void) {
    static const uint8_t none[4];
    for (uint32_t p = 0; p < 2u; p++) {
        uint32_t st = USB_STATUS(p), type = st & 3u, count = st >> 8 & 0xFFu;
        if (type != usb[p].type) {   // unplugged or replaced: release everything
            usb_keys_changed(usb[p].keys, usb[p].mods, none, 0);
            for (int b = 0; b < 3; b++)
                if ((usb[p].buttons >> b) & 1u) mouse_button(b == 0 ? MB_LEFT : b == 1 ? MB_RIGHT : MB_MIDDLE, 0);
            memset(usb[p].keys, 0, 4);
            usb[p].mods = usb[p].buttons = 0;
            usb[p].pos = USB_MOUSE_POS(p);
            usb[p].type = type;
            usb[p].count = count;
            continue;
        }
        if (type == 2u) {
            uint32_t pos = USB_MOUSE_POS(p);
            int dx = (int16_t)(pos - usb[p].pos), dy = (int16_t)((pos >> 16) - (usb[p].pos >> 16));
            usb[p].pos = pos;
            if (dx || dy) mouse_motion(dx, dy);
            uint8_t b = (uint8_t)(USB_MOUSE(p) & 7u);
            for (int i = 0; i < 3; i++)   // HID bit 0 left, 1 right, 2 middle
                if (((b ^ usb[p].buttons) >> i) & 1u)
                    mouse_button(i == 0 ? MB_LEFT : i == 1 ? MB_RIGHT : MB_MIDDLE, (b >> i) & 1);
            usb[p].buttons = b;
            continue;
        }
        if (type != 1u || count == usb[p].count) continue;
        usb[p].count = count;
        uint32_t k = USB_KEYS(p);
        uint8_t keys[4] = {(uint8_t)(k >> 8), (uint8_t)(k >> 16), (uint8_t)(k >> 24), (uint8_t)USB_KEYS2(p)};
        if (keys[0] == 0x01u) continue;   // ErrorRollOver
        usb_keys_changed(usb[p].keys, usb[p].mods, keys, (uint8_t)k);
        memcpy(usb[p].keys, keys, 4);
        usb[p].mods = (uint8_t)k;
    }
}

void event_poll(void) {
    poll_uart();
    usb_poll();
    m9b_digi_poll();
}

int event_init(void) { return 0; }

// main/key.c's arch hooks.
void arch_key_init(void) { keyd_fake_repeat = 1; }
void arch_key_close(void) {}
void arch_key_flush(void) {}
void arch_key_poll(void) { event_poll(); }

// -------------------------------------------------------------------------
// Start-up (arch/linux/init.c): traps and interrupts, the keyboard, mouse.
// -------------------------------------------------------------------------
#define IRQ_PENDING   (*(volatile uint32_t*)(M9B_CTRL + 0x4u))
#define IRQ_ENABLE    (*(volatile uint32_t*)(M9B_CTRL + 0x8u))
extern void trap_entry(void);   // ../../core/runtime/trap_entry.S

void arch_init_start(void) {
    asm volatile("csrw mtvec, %0" :: "r"(&trap_entry));
    IRQ_PENDING = 0xFu;
    IRQ_ENABLE |= 1u;   // vertical blank: the page flip (m9b_video.c)
    asm volatile("csrs mie, %0" :: "r"(1u << 11));
    asm volatile("csrs mstatus, %0" :: "r"(1u << 3));
}

void arch_init(void) {
    memset(&Mouse, 0, sizeof(Mouse));
    timer_init();
    key_init();
}

// -------------------------------------------------------------------------
// Joystick: none (arch/linux/joystick.c's interface, all idle).
// -------------------------------------------------------------------------
char joy_installed = 0, joy_present = 0;
int j_num_axes = 0, j_num_buttons = 0, joy_deadzone = 0;
int Joy_is_Sidewinder = 0;
joystick_device j_joystick[MAX_JOY_DEVS];
joystick_axis j_axis[MAX_AXES];
joystick_button j_button[MAX_BUTTONS];

int joy_init(void) { return 0; }
void joy_close(void) {}
void joy_flush(void) {}
int joy_get_button_state(int btn) { (void)btn; return 0; }
int joy_get_button_down_cnt(int btn) { (void)btn; return 0; }
fix joy_get_button_down_time(int btn) { (void)btn; return 0; }
void joy_get_pos(int *x, int *y) { *x = *y = 0; }
ubyte joystick_read_raw_axis(ubyte mask, int *axis) { (void)mask; (void)axis; return 0; }
int joy_get_scaled_reading(int raw, int axn) { (void)raw; (void)axn; return 0; }
void joy_set_cal_vals(int *axis_min, int *axis_center, int *axis_max) { (void)axis_min; (void)axis_center; (void)axis_max; }
void joy_get_cal_vals(int *axis_min, int *axis_center, int *axis_max) { (void)axis_min; (void)axis_center; (void)axis_max; }
int j_Get_joydev_axis_number(int all_axis_number) { (void)all_axis_number; return 0; }
int j_Get_joydev_button_number(int all_button_number) { (void)all_button_number; return 0; }
void joydefs_calibrate(void) {}
void joydefs_config(void) {}
