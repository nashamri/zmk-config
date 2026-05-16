/*
 * Portrait 90°-CW rotation status screen for 128×32 SSD1306
 * mounted vertically.
 *
 *   Top:    battery percentage  (e.g. "95%")
 *   Center: active layer name   (e.g. "Base", "Sym", "Nav")
 *   Bottom: output endpoint     ("USB" or "BT1+" / "BT1-" / "BT1?")
 */

#include <zmk/display/status_screen.h>
#include <zmk/display.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zephyr/drivers/display.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <lvgl.h>
#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ── Portrait rotation ───────────────────────────────────────────────────── */

static void portrait_to_landscape_90cw(const uint8_t *portrait, uint8_t *landscape)
{
    memset(landscape, 0, 512);
    for (int c = 0; c < 32; c++) {
        for (int r = 0; r < 128; r++) {
            if (!((portrait[c + (r >> 3) * 32] >> (r & 7)) & 1))
                continue;
            int lr = 31 - c;
            landscape[r + (lr >> 3) * 128] |= 1 << (lr & 7);
        }
    }
}

static const struct device *tux_display_dev;
static uint8_t landscape_buf[512];

static void tux_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    ARG_UNUSED(area);
    portrait_to_landscape_90cw((const uint8_t *)color_p, landscape_buf);
    const struct display_buffer_descriptor desc = {
        .buf_size = 512, .width = 128, .pitch = 128, .height = 32,
    };
    display_write(tux_display_dev, 0, 0, &desc, landscape_buf);
    lv_disp_flush_ready(drv);
}

static void tux_rounder_cb(lv_disp_drv_t *drv, lv_area_t *area)
{
    ARG_UNUSED(drv);
    area->y1 &= ~0x7;
    area->y2 |= 0x7;
}

/* ── Battery ─────────────────────────────────────────────────────────────── */

static lv_obj_t *battery_label;

struct batt_state { uint8_t level; };

static void battery_update(struct batt_state s)
{
    lv_label_set_text_fmt(battery_label, "%u%%", s.level);
}

static struct batt_state battery_get_state(const zmk_event_t *eh)
{
    return (struct batt_state){ .level = zmk_battery_state_of_charge() };
}

ZMK_DISPLAY_WIDGET_LISTENER(battery_listener, struct batt_state,
                             battery_update, battery_get_state)
ZMK_SUBSCRIPTION(battery_listener, zmk_battery_state_changed);

/* ── Layer ───────────────────────────────────────────────────────────────── */

static lv_obj_t *layer_label;

struct layer_state_s { zmk_keymap_layer_index_t index; const char *name; };

static void layer_update(struct layer_state_s s)
{
    if (s.name != NULL && strlen(s.name) > 0) {
        lv_label_set_text(layer_label, s.name);
    } else {
        lv_label_set_text_fmt(layer_label, "L%u", s.index);
    }
}

static struct layer_state_s layer_get_state(const zmk_event_t *eh)
{
    zmk_keymap_layer_index_t idx = zmk_keymap_highest_layer_active();
    return (struct layer_state_s){
        .index = idx,
        .name  = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(idx)),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(layer_listener, struct layer_state_s,
                             layer_update, layer_get_state)
ZMK_SUBSCRIPTION(layer_listener, zmk_layer_state_changed);

/* ── Output / BT ─────────────────────────────────────────────────────────── */

static lv_obj_t *output_label;

struct out_state {
    bool usb;
    uint8_t bt_index;
    bool connected;
    bool open;
};

static void output_update(struct out_state s)
{
    if (s.usb) {
        lv_label_set_text(output_label, "USB");
    } else {
        const char *mark = s.open ? "?" : (s.connected ? "+" : "-");
        lv_label_set_text_fmt(output_label, "BT%u%s", s.bt_index + 1, mark);
    }
}

static struct out_state output_get_state(const zmk_event_t *eh)
{
    uint8_t idx = zmk_ble_active_profile_index();
    return (struct out_state){
        .usb       = (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB),
        .bt_index  = idx,
        .connected = zmk_ble_profile_is_connected(idx),
        .open      = zmk_ble_profile_is_open(idx),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(output_listener, struct out_state,
                             output_update, output_get_state)
ZMK_SUBSCRIPTION(output_listener, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(output_listener, zmk_ble_active_profile_changed);

/* ── Screen assembly ─────────────────────────────────────────────────────── */

lv_obj_t *zmk_display_status_screen(void)
{
    tux_display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

    lv_disp_t *disp = lv_disp_get_default();
    disp->driver->hor_res      = 32;
    disp->driver->ver_res      = 128;
    disp->driver->full_refresh = 1;
    disp->driver->flush_cb     = tux_flush_cb;
    disp->driver->rounder_cb   = tux_rounder_cb;

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    battery_label = lv_label_create(screen);
    lv_obj_align(battery_label, LV_ALIGN_TOP_MID, 0, 0);

    layer_label = lv_label_create(screen);
    lv_obj_align(layer_label, LV_ALIGN_CENTER, 0, 0);

    output_label = lv_label_create(screen);
    lv_obj_align(output_label, LV_ALIGN_BOTTOM_MID, 0, 0);

    battery_listener_init();
    layer_listener_init();
    output_listener_init();

    return screen;
}
