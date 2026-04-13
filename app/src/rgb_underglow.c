/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <math.h>
#include <stdlib.h>

#include <zephyr/logging/log.h>

#include <zephyr/drivers/led_strip.h>
#include <drivers/ext_power.h>

#include <zmk/rgb_underglow.h>

#include <zmk/activity.h>
#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/workqueue.h>
#include <zmk/endpoints.h>
#include <zmk/hid_indicators_types.h>
#include <zmk/hid_indicators.h>
#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#endif
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#endif

#if IS_ENABLED(CONFIG_EXPERIMENTAL_RGB_LAYER)
#include <zmk/rgb_underglow_layer.h>
#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <drivers/behavior.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_underglow)

#error "A zmk,underglow chosen node must be declared"

#endif

#define STRIP_CHOSEN DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_underglow_layer) && IS_ENABLED(CONFIG_EXPERIMENTAL_RGB_LAYER)
#define UNDERGLOW_LAYER_ENABLED 1
static int zmk_rgb_underglow_apply_merged_rgbmap(void);
static void zmk_rgb_underglow_set_layer(uint8_t layer, bool wakeup);
#endif

#define HUE_MAX 360
#define SAT_MAX 100
#define BRT_MAX 100

BUILD_ASSERT(CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN <= CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX,
             "ERROR: RGB underglow maximum brightness is less than minimum brightness");

enum rgb_underglow_effect {
    UNDERGLOW_EFFECT_SOLID,
    UNDERGLOW_EFFECT_BREATHE,
    UNDERGLOW_EFFECT_SPECTRUM,
    UNDERGLOW_EFFECT_SWIRL,
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    UNDERGLOW_EFFECT_PER_KEY_ONLY, // Per-key colors only, no background
#endif
    UNDERGLOW_EFFECT_OFF, // All LEDs off
    UNDERGLOW_EFFECT_NUMBER
};

struct rgb_underglow_state {
    struct zmk_led_hsb color;
    uint8_t animation_speed;
    uint8_t current_effect;
    uint16_t animation_step;
    bool on;
    /* RGB_STATUS popup — tap-Magic indicator (battery/HID/BLE/USB/layer).
     * status_active gates whether the status_pixels buffer is composited
     * over the regular animation frame. animation_step is in 25 ms ticks:
     *   0..20   = fade-in (500 ms)
     *   20..320 = full status (~8 s)
     *   320..400 = fade-out (2 s)
     *   >400    = off
     */
    bool status_active;
    uint16_t status_animation_step;
};

static const struct device *led_strip;

static struct led_rgb pixels[STRIP_NUM_PIXELS];
static struct led_rgb status_pixels[STRIP_NUM_PIXELS];

static struct rgb_underglow_state state;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

static struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb) {
    hsb.b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN +
            (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX - CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN) * hsb.b / BRT_MAX;
    return hsb;
}

static struct zmk_led_hsb hsb_scale_zero_max(struct zmk_led_hsb hsb) {
    hsb.b = hsb.b * CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX / BRT_MAX;
    return hsb;
}

static struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb) {
    float r = 0, g = 0, b = 0;

    uint8_t i = hsb.h / 60;
    float v = hsb.b / ((float)BRT_MAX);
    float s = hsb.s / ((float)SAT_MAX);
    float f = hsb.h / ((float)HUE_MAX) * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    switch (i % 6) {
    case 0:
        r = v;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = v;
        b = p;
        break;
    case 2:
        r = p;
        g = v;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = v;
        break;
    case 4:
        r = t;
        g = p;
        b = v;
        break;
    case 5:
        r = v;
        g = p;
        b = q;
        break;
    }

    struct led_rgb rgb = {r : r * 255, g : g * 255, b : b * 255};

    return rgb;
}

static void zmk_rgb_underglow_effect_solid(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = hsb_to_rgb(hsb_scale_min_max(state.color));
    }
}

static void zmk_rgb_underglow_effect_breathe(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.b = abs(state.animation_step - 1200) / 12;

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }

    state.animation_step += state.animation_speed * 10;

    if (state.animation_step > 2400) {
        state.animation_step = 0;
    }
}

static void zmk_rgb_underglow_effect_spectrum(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = state.animation_step;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.animation_speed;
    state.animation_step = state.animation_step % HUE_MAX;
}

static void zmk_rgb_underglow_effect_swirl(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = (HUE_MAX / STRIP_NUM_PIXELS * i + state.animation_step) % HUE_MAX;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.animation_speed * 2;
    state.animation_step = state.animation_step % HUE_MAX;
}

static void zmk_rgb_underglow_tick(struct k_work *work) {
    switch (state.current_effect) {
    case UNDERGLOW_EFFECT_SOLID:
        zmk_rgb_underglow_effect_solid();
        break;
    case UNDERGLOW_EFFECT_BREATHE:
        zmk_rgb_underglow_effect_breathe();
        break;
    case UNDERGLOW_EFFECT_SPECTRUM:
        zmk_rgb_underglow_effect_spectrum();
        break;
    case UNDERGLOW_EFFECT_SWIRL:
        zmk_rgb_underglow_effect_swirl();
        break;
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    case UNDERGLOW_EFFECT_PER_KEY_ONLY:
        memset(pixels, 0, sizeof(struct led_rgb) * STRIP_NUM_PIXELS);
        break;
#endif
    case UNDERGLOW_EFFECT_OFF:
        memset(pixels, 0, sizeof(struct led_rgb) * STRIP_NUM_PIXELS);
        break;
    }

    // Always overlay per-key colors on top of whatever effect just ran
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (state.current_effect != UNDERGLOW_EFFECT_OFF) {
        zmk_rgb_underglow_apply_merged_rgbmap();
    }
#endif

    // RGB_STATUS popup: blend status_pixels over whatever we just composited.
    // Defined further down; no-op on RH half or when status_active is false.
    extern int16_t zmk_rgb_underglow_status_blend(void);
    int16_t blend = zmk_rgb_underglow_status_blend();
    if (blend > 0) {
        int16_t blend_l = blend;
        int16_t blend_r = 256 - blend;
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            pixels[i].r = ((status_pixels[i].r * blend_l) >> 8) + ((pixels[i].r * blend_r) >> 8);
            pixels[i].g = ((status_pixels[i].g * blend_l) >> 8) + ((pixels[i].g * blend_r) >> 8);
            pixels[i].b = ((status_pixels[i].b * blend_l) >> 8) + ((pixels[i].b * blend_r) >> 8);
        }
    }

    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }
}

K_WORK_DEFINE(underglow_tick_work, zmk_rgb_underglow_tick);

static void zmk_rgb_underglow_tick_handler(struct k_timer *timer) {
    if (!state.on) {
        return;
    }

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_tick_work);
}

K_TIMER_DEFINE(underglow_tick, zmk_rgb_underglow_tick_handler, NULL);

#if IS_ENABLED(CONFIG_SETTINGS)
static int rgb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    int rc;

    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(state)) {
            return -EINVAL;
        }

        rc = read_cb(cb_arg, &state, sizeof(state));
        if (rc >= 0) {
            if (state.on) {
                k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));
            }

            return 0;
        }

        return rc;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(rgb_underglow, "rgb/underglow", NULL, rgb_settings_set, NULL, NULL);

static void zmk_rgb_underglow_save_state_work(struct k_work *_work) {
    settings_save_one("rgb/underglow/state", &state, sizeof(state));
}

static struct k_work_delayable underglow_save_work;
#endif

static int zmk_rgb_underglow_init(void) {
    led_strip = DEVICE_DT_GET(STRIP_CHOSEN);

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (!device_is_ready(ext_power)) {
        LOG_ERR("External power device \"%s\" is not ready", ext_power->name);
        return -ENODEV;
    }
#endif

    state = (struct rgb_underglow_state){
        color : {
            h : CONFIG_ZMK_RGB_UNDERGLOW_HUE_START,
            s : CONFIG_ZMK_RGB_UNDERGLOW_SAT_START,
            b : CONFIG_ZMK_RGB_UNDERGLOW_BRT_START,
        },
        animation_speed : CONFIG_ZMK_RGB_UNDERGLOW_SPD_START,
        current_effect : CONFIG_ZMK_RGB_UNDERGLOW_EFF_START,
        animation_step : 0,
        on : IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_ON_START)
    };

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&underglow_save_work, zmk_rgb_underglow_save_state_work);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
    state.on = zmk_usb_is_powered();
#endif

    if (state.on) {
        k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));
    }

    return 0;
}

int zmk_rgb_underglow_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    int ret = k_work_reschedule(&underglow_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
#else
    return 0;
#endif
}

int zmk_rgb_underglow_get_state(bool *on_off) {
    if (!led_strip)
        return -ENODEV;

    *on_off = state.on;
    return 0;
}

int zmk_rgb_underglow_on(void) {
    if (!led_strip)
        return -ENODEV;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_enable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to enable EXT_POWER: %d", rc);
        }
    }
#endif

    state.on = true;
    state.animation_step = 0;
    k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));

    return zmk_rgb_underglow_save_state();
}

static void zmk_rgb_underglow_off_handler(struct k_work *work) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }

    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
}

K_WORK_DEFINE(underglow_off_work, zmk_rgb_underglow_off_handler);

int zmk_rgb_underglow_off(void) {
    if (!led_strip)
        return -ENODEV;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_disable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to disable EXT_POWER: %d", rc);
        }
    }
#endif

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_off_work);

    k_timer_stop(&underglow_tick);
    state.on = false;

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_calc_effect(int direction) {
    return (state.current_effect + UNDERGLOW_EFFECT_NUMBER + direction) % UNDERGLOW_EFFECT_NUMBER;
}

int zmk_rgb_underglow_select_effect(int effect) {
    if (!led_strip)
        return -ENODEV;

    if (effect < 0 || effect >= UNDERGLOW_EFFECT_NUMBER) {
        return -EINVAL;
    }

    state.current_effect = effect;
    state.animation_step = 0;

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_cycle_effect(int direction) {
    return zmk_rgb_underglow_select_effect(zmk_rgb_underglow_calc_effect(direction));
}

int zmk_rgb_underglow_toggle(void) {
    return state.on ? zmk_rgb_underglow_off() : zmk_rgb_underglow_on();
}

int zmk_rgb_underglow_set_hsb(struct zmk_led_hsb color) {
    if (color.h > HUE_MAX || color.s > SAT_MAX || color.b > BRT_MAX) {
        return -ENOTSUP;
    }

    state.color = color;

    return 0;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_hue(int direction) {
    struct zmk_led_hsb color = state.color;

    color.h += HUE_MAX + (direction * CONFIG_ZMK_RGB_UNDERGLOW_HUE_STEP);
    color.h %= HUE_MAX;

    return color;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_sat(int direction) {
    struct zmk_led_hsb color = state.color;

    int s = color.s + (direction * CONFIG_ZMK_RGB_UNDERGLOW_SAT_STEP);
    if (s < 0) {
        s = 0;
    } else if (s > SAT_MAX) {
        s = SAT_MAX;
    }
    color.s = s;

    return color;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_brt(int direction) {
    struct zmk_led_hsb color = state.color;

    int b = color.b + (direction * CONFIG_ZMK_RGB_UNDERGLOW_BRT_STEP);
    color.b = CLAMP(b, 0, BRT_MAX);

    return color;
}

int zmk_rgb_underglow_change_hue(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_hue(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_sat(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_sat(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_brt(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_brt(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_spd(int direction) {
    if (!led_strip)
        return -ENODEV;

    if (state.animation_speed == 1 && direction < 0) {
        return 0;
    }

    state.animation_speed += direction;

    if (state.animation_speed > 5) {
        state.animation_speed = 5;
    }

    return zmk_rgb_underglow_save_state();
}

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE) ||                                          \
    IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB) || IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
struct rgb_underglow_sleep_state {
    bool is_awake;
    bool rgb_state_before_sleeping;
};

static int rgb_underglow_auto_state(bool target_wake_state) {
    static struct rgb_underglow_sleep_state sleep_state = {
        is_awake : true,
        rgb_state_before_sleeping : false
    };

    // wake up event while awake, or sleep event while sleeping -> no-op
    if (target_wake_state == sleep_state.is_awake) {
        return 0;
    }
    sleep_state.is_awake = target_wake_state;

    if (sleep_state.is_awake) {
        if (sleep_state.rgb_state_before_sleeping) {
            return zmk_rgb_underglow_on();
        } else {
            return zmk_rgb_underglow_off();
        }
    } else {
        sleep_state.rgb_state_before_sleeping = state.on;
        return zmk_rgb_underglow_off();
    }
}

static int rgb_underglow_event_listener(const zmk_event_t *eh) {

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
    if (as_zmk_activity_state_changed(eh)) {
        return rgb_underglow_auto_state(zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        return rgb_underglow_auto_state(zmk_usb_is_powered());
    }
#endif

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* layer_state_changed is only raised on the central half. Peripheral
     * receives layer state via the split comms path (handled elsewhere). */
    if (as_zmk_layer_state_changed(eh)) {
        uint8_t layer = zmk_keymap_highest_layer_active();
        zmk_rgb_underglow_set_layer(layer, true);
        return 0;
    }
#endif

    return -ENOTSUP;
}

ZMK_LISTENER(rgb_underglow, rgb_underglow_event_listener);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_activity_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_usb_conn_state_changed);
#endif

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* zmk_layer_state_changed only exists on the split central. Peripheral gets
 * its layer state via the split transport callback that writes into
 * peripheral_layers_state(). */
ZMK_SUBSCRIPTION(rgb_underglow, zmk_layer_state_changed);
#endif

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)

static struct led_rgb hex_to_rgb(uint8_t r, uint8_t g, uint8_t b) {
    struct zmk_led_hsb hsb = state.color;
    return (struct led_rgb){
        r : (hsb.b * (r)) / 0xff,
        g : (hsb.b * (g)) / 0xff,
        b : (hsb.b * (b)) / 0xff
    };
}

static int zmk_rgb_underglow_apply_merged_rgbmap(void) {
    int rc = 0;
    size_t len = 0;
    uint8_t active_layers[ZMK_KEYMAP_LAYERS_LEN];
    /* zmk_keymap_layer_state() only exists on the split central. On the
     * peripheral we fall back to the mirrored state replicated from central. */
    uint32_t layer_state = rgb_underglow_layers_state();

    for (uint8_t layer = ZMK_KEYMAP_LAYERS_LEN - 1; layer > 0; layer--) {
        if ((layer_state & (BIT(layer))) == (BIT(layer))) {
            active_layers[len++] = layer;
        }
    }
    active_layers[len++] = 0; // default layer

    if (rgb_underglow_get_bindings(active_layers[0]) == NULL) {
        return 0;
    }

    for (int pixel = 0; pixel < STRIP_NUM_PIXELS; pixel++) {
        uint8_t midx = rgb_pixel_lookup(pixel);
        int color = 0;
        bool is_transparent = true;

        if (midx < ZMK_KEYMAP_LEN) {
            for (int layer = 0; layer < len; layer++) {
                /* Live overlay edited via Studio rgb subsystem wins over the
                 * DT-defined binding chain — but only when the layer isn't
                 * marked transparent. */
                uint32_t live_color;
                if (zmk_rgb_underglow_studio_lookup(active_layers[layer], midx, &live_color)) {
                    color = (int)live_color;
                    if (((color >> 24) & 0xFF) == 0xFF) {
                        color = 0;
                        continue;
                    }
                    is_transparent = false;
                    break;
                }

                const struct zmk_behavior_binding *bindings =
                    rgb_underglow_get_bindings(active_layers[layer]);
                if (bindings != NULL) {
                    const struct device *dev =
                        zmk_behavior_get_binding(bindings[midx].behavior_dev);
                    if (dev != NULL) {
                        const struct behavior_driver_api *api =
                            (const struct behavior_driver_api *)dev->api;
                        if (api->binding_pressed != NULL) {
                            struct zmk_behavior_binding_event event = {.position = midx,
                                                                       .layer =
                                                                           active_layers[layer],
                                                                       .timestamp = k_uptime_get()};
                            color = api->binding_pressed(
                                (struct zmk_behavior_binding *)&bindings[midx], event);
                            if (color == ZMK_BEHAVIOR_TRANSPARENT) {
                                color = 0;
                                continue;
                            }
                            is_transparent = false;
                        }
                    }
                }
                break;
            }

            // Skip transparent keys — leave base animation intact
            if (is_transparent || ((color >> 24) & 0xFF) == 0xFF) {
                continue;
            }

            // Extract per-key effect from upper 8 bits, RGB from lower 24
            int effect_mode = (color >> 24) & 0xFF;
            int rgb_color = color & 0xFFFFFF;
            struct led_rgb pixel_color =
                hex_to_rgb((rgb_color >> 16) & 0xFF, (rgb_color >> 8) & 0xFF, rgb_color & 0xFF);

            switch (effect_mode) {
            case 1: { // breathe
                int brt = abs((int)(state.animation_step % 2400) - 1200) * 255 / 1200;
                pixel_color.r = pixel_color.r * brt / 255;
                pixel_color.g = pixel_color.g * brt / 255;
                pixel_color.b = pixel_color.b * brt / 255;
                break;
            }
            case 2: { // pulse
                int phase = state.animation_step % 1200;
                if (phase > 300) {
                    pixel_color.r = 0;
                    pixel_color.g = 0;
                    pixel_color.b = 0;
                }
                break;
            }
            case 4: // dim
                pixel_color.r /= 2;
                pixel_color.g /= 2;
                pixel_color.b /= 2;
                break;
            default:
                break;
            }

            pixels[pixel] = pixel_color;

            if ((color & 0xFFFFFF) > 0) {
                rc = 1;
            }
        }
    }
    return rc;
}

static void zmk_rgb_underglow_set_layer(uint8_t layer, bool wakeup) {
    if (state.on) {
        // Tick handler will apply overlay on next frame
        if (!k_work_is_pending(&underglow_tick_work)) {
            k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_tick_work);
        }
    } else if (wakeup) {
        zmk_rgb_underglow_on();
    }
}

#endif /* IS_ENABLED(UNDERGLOW_LAYER_ENABLED) */

/* ── RGB_STATUS indicators (ported from moergo-sc/darknao Glove80 forks) ──
 *
 * When the Magic key is tapped, the `&rgb_ug RGB_STATUS` binding calls
 * zmk_rgb_underglow_status(). That kicks off a 10 s popup on the LEDs that
 * shows:
 *   - LH battery level (green/yellow/red bar)
 *   - RH battery level (fetched from peripheral over BLE split, red bar if
 *     disconnected)
 *   - Caps/Num/Scroll lock state (red pixels where set)
 *   - Active layer (magenta)
 *   - BLE profile state (white=active, green=connected, red=paired,
 *     lilac=unused)
 *   - USB HID state (white=active, green=connected, red=powered-only,
 *     lilac=disconnected)
 *   - Output transport fallback (red = preferred transport inactive)
 *
 * The popup is only rendered on the half that has the `underglow_indicators`
 * DT node (typically the LH central). On the peripheral half the status
 * path compiles to a no-op.
 */
#define UNDERGLOW_INDICATORS DT_PATH(underglow_indicators)

#if DT_NODE_HAS_STATUS(UNDERGLOW_INDICATORS, okay)

#define UNDERGLOW_INDICATORS_ENABLED 1

static const uint8_t underglow_layer_state[] = DT_PROP(UNDERGLOW_INDICATORS, layer_state);
static const uint8_t underglow_ble_state[] = DT_PROP(UNDERGLOW_INDICATORS, ble_state);
static const uint8_t underglow_bat_lhs[] = DT_PROP(UNDERGLOW_INDICATORS, bat_lhs);
static const uint8_t underglow_bat_rhs[] = DT_PROP(UNDERGLOW_INDICATORS, bat_rhs);

#define STATUS_RGB(R, G, B)                                                                        \
    ((struct led_rgb){                                                                             \
        r : (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX * (R)) / 0xff,                                       \
        g : (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX * (G)) / 0xff,                                       \
        b : (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX * (B)) / 0xff                                        \
    })

static const struct led_rgb status_red = STATUS_RGB(0xff, 0x00, 0x00);
static const struct led_rgb status_yellow = STATUS_RGB(0xff, 0xff, 0x00);
static const struct led_rgb status_green = STATUS_RGB(0x00, 0xff, 0x00);
static const struct led_rgb status_dull_green = STATUS_RGB(0x00, 0xff, 0x68);
static const struct led_rgb status_magenta = STATUS_RGB(0xff, 0x00, 0xff);
static const struct led_rgb status_white = STATUS_RGB(0xff, 0xff, 0xff);
static const struct led_rgb status_lilac = STATUS_RGB(0x6b, 0x1f, 0xce);

static void status_battery_bar(int bat_level, const uint8_t *addresses, size_t len) {
    struct led_rgb color = (bat_level > 40)   ? status_green
                           : (bat_level > 20) ? status_yellow
                                              : status_red;
    for (size_t i = 0; i < len; i++) {
        int min_level = (i * 100) / (len - 1);
        if (bat_level >= min_level) {
            status_pixels[addresses[i]] = color;
        }
    }
}

static void status_fill(struct led_rgb color, const uint8_t *addresses, size_t len) {
    for (size_t i = 0; i < len; i++) {
        status_pixels[addresses[i]] = color;
    }
}

static int16_t zmk_rgb_underglow_generate_status(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        status_pixels[i] = (struct led_rgb){0};
    }

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    status_battery_bar(zmk_battery_state_of_charge(), underglow_bat_lhs,
                       DT_PROP_LEN(UNDERGLOW_INDICATORS, bat_lhs));
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    uint8_t peripheral_level = 0;
    int rc = zmk_split_central_get_peripheral_battery_level(0, &peripheral_level);
    if (rc == 0) {
        status_battery_bar(peripheral_level, underglow_bat_rhs,
                           DT_PROP_LEN(UNDERGLOW_INDICATORS, bat_rhs));
    } else if (rc == -ENODEV || rc == -ENOTCONN) {
        status_fill(status_red, underglow_bat_rhs, DT_PROP_LEN(UNDERGLOW_INDICATORS, bat_rhs));
    }
#endif
#endif

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    zmk_hid_indicators_t led_flags = zmk_hid_indicators_get_current_profile();
    if (led_flags & HID_INDICATOR_CAPS_LOCK)
        status_pixels[DT_PROP(UNDERGLOW_INDICATORS, capslock)] = status_red;
    if (led_flags & HID_INDICATOR_NUM_LOCK)
        status_pixels[DT_PROP(UNDERGLOW_INDICATORS, numlock)] = status_red;
    if (led_flags & HID_INDICATOR_SCROLL_LOCK)
        status_pixels[DT_PROP(UNDERGLOW_INDICATORS, scrolllock)] = status_red;
#endif

    for (uint8_t i = 0; i < DT_PROP_LEN(UNDERGLOW_INDICATORS, layer_state); i++) {
        if (zmk_keymap_layer_active(i))
            status_pixels[underglow_layer_state[i]] = status_magenta;
    }

    struct zmk_endpoint_instance active_endpoint = zmk_endpoint_get_selected();
    if (zmk_endpoint_get_preferred_transport() != active_endpoint.transport)
        status_pixels[DT_PROP(UNDERGLOW_INDICATORS, output_fallback)] = status_red;

#if IS_ENABLED(CONFIG_ZMK_BLE)
    int active_ble = zmk_ble_active_profile_index();
    for (uint8_t i = 0;
         i < MIN(ZMK_BLE_PROFILE_COUNT, DT_PROP_LEN(UNDERGLOW_INDICATORS, ble_state)); i++) {
        // Upstream Z4.1 exposes per-index is_open/is_connected; derive darknao's
        // 3-state status (0=unused, 1=paired, 2=connected) on the fly.
        bool open = zmk_ble_profile_is_open(i);
        bool connected = zmk_ble_profile_is_connected(i);
        uint8_t px = underglow_ble_state[i];
        if (connected && active_endpoint.transport == ZMK_TRANSPORT_BLE && active_ble == i) {
            status_pixels[px] = status_white;
        } else if (connected) {
            status_pixels[px] = status_dull_green;
        } else if (!open) {
            status_pixels[px] = status_red; // paired but not connected
        } else {
            status_pixels[px] = status_lilac; // unused slot
        }
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
    enum zmk_usb_conn_state usb_state = zmk_usb_get_conn_state();
    uint8_t usb_px = DT_PROP(UNDERGLOW_INDICATORS, usb_state);
    if (usb_state == ZMK_USB_CONN_HID && active_endpoint.transport == ZMK_TRANSPORT_USB) {
        status_pixels[usb_px] = status_white;
    } else if (usb_state == ZMK_USB_CONN_HID) {
        status_pixels[usb_px] = status_dull_green;
    } else if (usb_state == ZMK_USB_CONN_POWERED) {
        status_pixels[usb_px] = status_red;
    } else if (usb_state == ZMK_USB_CONN_NONE) {
        status_pixels[usb_px] = status_lilac;
    }
#endif

    // 500 ms fade-in, ~8 s hold, 2 s fade-out. Step is 25 ms.
    int16_t blend = 256;
    if (state.status_animation_step < (500 / 25)) {
        blend = (state.status_animation_step * 256) / (500 / 25);
    } else if (state.status_animation_step > (8000 / 25)) {
        blend = 256 - (((state.status_animation_step - (8000 / 25)) * 256) / (2000 / 25));
    }
    if (blend < 0)
        blend = 0;
    if (blend > 256)
        blend = 256;
    return blend;
}

int16_t zmk_rgb_underglow_status_blend(void) {
    if (!state.status_active) {
        return 0;
    }
    return zmk_rgb_underglow_generate_status();
}

static void zmk_rgb_underglow_status_update(struct k_timer *timer);
K_TIMER_DEFINE(underglow_status_update_timer, zmk_rgb_underglow_status_update, NULL);

static void zmk_rgb_underglow_status_update(struct k_timer *timer) {
    if (!state.status_active)
        return;
    state.status_animation_step++;
    if (state.status_animation_step > (10000 / 25)) {
        state.status_active = false;
        k_timer_stop(&underglow_status_update_timer);
    }
    if (!k_work_is_pending(&underglow_tick_work)) {
        k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_tick_work);
    }
}

int zmk_rgb_underglow_status(void) {
    if (!state.status_active) {
        state.status_animation_step = 0;
    } else if (state.status_animation_step > (500 / 25)) {
        // Retap while already fading out → snap back to full brightness.
        state.status_animation_step = 500 / 25;
    }
    state.status_active = true;
    // Ensure the tick loop is running so the blend gets composited.
    if (!state.on) {
        k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(25));
    }
    k_timer_start(&underglow_status_update_timer, K_NO_WAIT, K_MSEC(25));
    return 0;
}

#else /* UNDERGLOW_INDICATORS node not present (e.g. RH peripheral) */

int16_t zmk_rgb_underglow_status_blend(void) { return 0; }

int zmk_rgb_underglow_status(void) { return -ENOTSUP; }

#endif /* DT_NODE_HAS_STATUS(UNDERGLOW_INDICATORS, okay) */

SYS_INIT(zmk_rgb_underglow_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
