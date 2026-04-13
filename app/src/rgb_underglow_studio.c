/*
 * AuroraKey per-key RGB live-edit overlay.
 *
 * Sits on top of the DT-defined zmk_rgbmap and lets the Studio RPC layer
 * stage / commit / discard per-key colour overrides at runtime. Rendering
 * (rgb_underglow.c) consults zmk_rgb_underglow_studio_lookup() before falling
 * back to the DT-driven binding chain.
 *
 * Wire format for `color`: 0xEERRGGBB where EE encodes the per-key effect
 * (see rgb_underglow.c — 0=solid, 1=breathe, 2=pulse, 4=dim, 0xFF=transparent).
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/rgb_underglow_layer.h>

#define LAYERS ZMK_KEYMAP_LAYERS_LEN
#define KEYS   ZMK_KEYMAP_LEN
#define MASK_BYTES ((KEYS + 7) / 8)

static uint32_t live_colors[LAYERS][KEYS];
static uint8_t  live_set_mask[LAYERS][MASK_BYTES];
static uint8_t  layer_transparent[LAYERS];

static uint32_t pending_colors[LAYERS][KEYS];
static uint8_t  pending_set_mask[LAYERS][MASK_BYTES];
static uint8_t  pending_transparent[LAYERS];
static uint8_t  pending_transparent_dirty[LAYERS];

static inline bool mask_get(const uint8_t *mask, uint32_t key) {
    return (mask[key / 8] >> (key % 8)) & 1u;
}
static inline void mask_set(uint8_t *mask, uint32_t key) {
    mask[key / 8] |= (uint8_t)(1u << (key % 8));
}
static inline void mask_clear_all(uint8_t *mask) { memset(mask, 0, MASK_BYTES); }

/* Resolve a Studio-side persistent layer_id → runtime index by scanning the
 * keymap. Returns -1 if not present. */
static int layer_id_to_index(uint32_t layer_id) {
    for (int i = 0; i < LAYERS; i++) {
        if (zmk_keymap_layer_index_to_id((zmk_keymap_layer_index_t)i) ==
            (zmk_keymap_layer_id_t)layer_id) {
            return i;
        }
    }
    return -1;
}

/* ── Public API consumed by the rgb subsystem RPC handlers ─────────────── */

int zmk_rgb_underglow_layer_stage_set(uint32_t layer_id, uint32_t key_pos, uint32_t color) {
    int idx = layer_id_to_index(layer_id);
    if (idx < 0) return -EINVAL;
    if (key_pos >= KEYS) return -ERANGE;
    pending_colors[idx][key_pos] = color;
    mask_set(pending_set_mask[idx], key_pos);
    return 0;
}

int zmk_rgb_underglow_layer_set_transparent(uint32_t layer_id, bool transparent) {
    int idx = layer_id_to_index(layer_id);
    if (idx < 0) return -EINVAL;
    pending_transparent[idx] = transparent ? 1u : 0u;
    pending_transparent_dirty[idx] = 1u;
    return 0;
}

int zmk_rgb_underglow_layer_get_color(uint32_t layer_id, uint32_t key_pos, uint32_t *out) {
    int idx = layer_id_to_index(layer_id);
    if (idx < 0) return -EINVAL;
    if (key_pos >= KEYS) return -ERANGE;
    if (mask_get(pending_set_mask[idx], key_pos)) {
        *out = pending_colors[idx][key_pos];
        return 0;
    }
    if (mask_get(live_set_mask[idx], key_pos)) {
        *out = live_colors[idx][key_pos];
        return 0;
    }
    return -ENOENT;
}

bool zmk_rgb_underglow_layer_is_transparent(uint32_t layer_id) {
    int idx = layer_id_to_index(layer_id);
    if (idx < 0) return false;
    if (pending_transparent_dirty[idx]) return pending_transparent[idx] != 0;
    return layer_transparent[idx] != 0;
}

void zmk_rgb_underglow_layer_clear(uint32_t layer_id) {
    int idx = layer_id_to_index(layer_id);
    if (idx < 0) return;
    mask_clear_all(live_set_mask[idx]);
    mask_clear_all(pending_set_mask[idx]);
}

int zmk_rgb_underglow_layer_save(void) {
    for (int l = 0; l < LAYERS; l++) {
        for (int k = 0; k < KEYS; k++) {
            if (mask_get(pending_set_mask[l], k)) {
                live_colors[l][k] = pending_colors[l][k];
                mask_set(live_set_mask[l], k);
            }
        }
        if (pending_transparent_dirty[l]) {
            layer_transparent[l] = pending_transparent[l];
        }
        mask_clear_all(pending_set_mask[l]);
        pending_transparent_dirty[l] = 0;
    }
    int rc = settings_save_one("rgb/colors", live_colors, sizeof(live_colors));
    if (!rc) rc = settings_save_one("rgb/mask", live_set_mask, sizeof(live_set_mask));
    if (!rc) rc = settings_save_one("rgb/trans", layer_transparent, sizeof(layer_transparent));
    if (rc) LOG_ERR("rgb settings save failed: %d", rc);
    return rc;
}

void zmk_rgb_underglow_layer_discard(void) {
    for (int l = 0; l < LAYERS; l++) {
        mask_clear_all(pending_set_mask[l]);
        pending_transparent_dirty[l] = 0;
    }
}

void zmk_rgb_underglow_layer_reset_all(void) {
    memset(live_colors, 0, sizeof(live_colors));
    memset(live_set_mask, 0, sizeof(live_set_mask));
    memset(layer_transparent, 0, sizeof(layer_transparent));
    zmk_rgb_underglow_layer_discard();
    settings_save_one("rgb/colors", live_colors, sizeof(live_colors));
    settings_save_one("rgb/mask", live_set_mask, sizeof(live_set_mask));
    settings_save_one("rgb/trans", layer_transparent, sizeof(layer_transparent));
}

/* ── Render-path lookup (called from rgb_underglow.c) ──────────────────── */

bool zmk_rgb_underglow_studio_lookup(uint8_t layer_index, uint32_t key_pos, uint32_t *out) {
    if (layer_index >= LAYERS || key_pos >= KEYS) return false;
    if (mask_get(pending_set_mask[layer_index], key_pos)) {
        *out = pending_colors[layer_index][key_pos];
        return true;
    }
    if (mask_get(live_set_mask[layer_index], key_pos)) {
        *out = live_colors[layer_index][key_pos];
        return true;
    }
    return false;
}

bool zmk_rgb_underglow_studio_layer_transparent(uint8_t layer_index) {
    if (layer_index >= LAYERS) return false;
    return layer_transparent[layer_index] != 0;
}

/* ── Settings load on boot ─────────────────────────────────────────────── */

static int rgb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (settings_name_steq(name, "colors", &next) && !next) {
        return read_cb(cb_arg, live_colors, MIN(len, sizeof(live_colors))) < 0 ? -EINVAL : 0;
    }
    if (settings_name_steq(name, "mask", &next) && !next) {
        return read_cb(cb_arg, live_set_mask, MIN(len, sizeof(live_set_mask))) < 0 ? -EINVAL : 0;
    }
    if (settings_name_steq(name, "trans", &next) && !next) {
        return read_cb(cb_arg, layer_transparent, MIN(len, sizeof(layer_transparent))) < 0 ? -EINVAL : 0;
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(rgb_studio, "rgb", NULL, rgb_settings_set, NULL, NULL);
