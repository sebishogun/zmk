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
#define KEYS ZMK_KEYMAP_LEN
#define MASK_BYTES ((KEYS + 7) / 8)

static uint32_t live_colors[LAYERS][KEYS];
static uint8_t live_set_mask[LAYERS][MASK_BYTES];
static uint8_t layer_transparent[LAYERS];

static uint32_t pending_colors[LAYERS][KEYS];
static uint8_t pending_set_mask[LAYERS][MASK_BYTES];
static uint8_t pending_transparent[LAYERS];
static uint8_t pending_transparent_dirty[LAYERS];

static inline bool mask_get(const uint8_t *mask, uint32_t key) {
    return (mask[key / 8] >> (key % 8)) & 1u;
}
static inline void mask_set(uint8_t *mask, uint32_t key) {
    mask[key / 8] |= (uint8_t)(1u << (key % 8));
}
static inline void mask_clear_one(uint8_t *mask, uint32_t key) {
    mask[key / 8] &= (uint8_t) ~(1u << (key % 8));
}
static inline void mask_clear_all(uint8_t *mask) { memset(mask, 0, MASK_BYTES); }

/* Resolve a Studio-side persistent layer_id → runtime index by scanning the
 * keymap. Returns -1 if not present.
 *
 * Peripheral builds don't link zmk_keymap_layer_index_to_id — it lives on the
 * central only. Central forwards the ALREADY-resolved index across the BLE
 * split (see split/bluetooth/central.c), so on the peripheral side we treat
 * the incoming value as a direct index. */
static int layer_id_to_index(uint32_t layer_id) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    for (int i = 0; i < LAYERS; i++) {
        if (zmk_keymap_layer_index_to_id((zmk_keymap_layer_index_t)i) ==
            (zmk_keymap_layer_id_t)layer_id) {
            return i;
        }
    }
    return -1;
#else
    return (layer_id < LAYERS) ? (int)layer_id : -1;
#endif
}

/* ── Public API consumed by the rgb subsystem RPC handlers ─────────────── */

int zmk_rgb_underglow_layer_stage_set(uint32_t layer_id, uint32_t key_pos, uint32_t color) {
    int idx = layer_id_to_index(layer_id);
    if (idx < 0)
        return -EINVAL;
    if (key_pos >= KEYS)
        return -ERANGE;
    pending_colors[idx][key_pos] = color;
    mask_set(pending_set_mask[idx], key_pos);
    return 0;
}

int zmk_rgb_underglow_layer_set_transparent(uint32_t layer_id, bool transparent) {
    int idx = layer_id_to_index(layer_id);
    if (idx < 0)
        return -EINVAL;
    pending_transparent[idx] = transparent ? 1u : 0u;
    pending_transparent_dirty[idx] = 1u;
    return 0;
}

int zmk_rgb_underglow_layer_get_color(uint32_t layer_id, uint32_t key_pos, uint32_t *out) {
    int idx = layer_id_to_index(layer_id);
    if (idx < 0)
        return -EINVAL;
    if (key_pos >= KEYS)
        return -ERANGE;
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
    if (idx < 0)
        return false;
    if (pending_transparent_dirty[idx])
        return pending_transparent[idx] != 0;
    return layer_transparent[idx] != 0;
}

void zmk_rgb_underglow_layer_clear(uint32_t layer_id) {
    int idx = layer_id_to_index(layer_id);
    if (idx < 0)
        return;
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
    if (!rc)
        rc = settings_save_one("rgb/mask", live_set_mask, sizeof(live_set_mask));
    if (!rc)
        rc = settings_save_one("rgb/trans", layer_transparent, sizeof(layer_transparent));
    if (rc)
        LOG_ERR("rgb settings save failed: %d", rc);
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
    if (layer_index >= LAYERS || key_pos >= KEYS)
        return false;
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
    if (layer_index >= LAYERS)
        return false;
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
        return read_cb(cb_arg, layer_transparent, MIN(len, sizeof(layer_transparent))) < 0 ? -EINVAL
                                                                                           : 0;
    }
    return 0;
}

/* h_commit fires after settings_load has filled live_colors[][] and
 * live_set_mask[][] from NVS. Without this reconciliation, a Studio
 * "Save changes" RPC permanently pins those keys' colours: every
 * subsequent firmware flash with a new DT-defined per-key colour map
 * is silently overridden on boot because the renderer's
 * studio_lookup() consults live_colors[] BEFORE falling through to
 * the DT bindings. NVS isn't wiped on UF2 flash (preserved by design,
 * same as BLE bonds), so the override outlives every code update.
 *
 * Strategy: walk the committed live overlay; for each set bit,
 * compare live_colors[L][K] against the current DT-baked binding's
 * param1 (which is the 0xEERRGGBB encoding the editor's codegen
 * emits). If they differ, the user has flashed a new colour for that
 * key and clearly intends it to take effect — drop the live mask bit
 * so DT wins on render. Pending overlays from the active USB session
 * still apply (pending_set_mask is checked first in studio_lookup),
 * so live editing during this boot continues to work; only the
 * committed-from-a-previous-firmware lock is cleared.
 *
 * Persists the reconciled mask back to NVS so the cleanup is one-shot
 * per flash; subsequent boots see the cleared state and skip cleanly. */
static int rgb_settings_commit(void) {
    bool dirty = false;
    for (int l = 0; l < LAYERS; l++) {
        const struct zmk_behavior_binding *bindings = rgb_underglow_get_bindings((uint8_t)l);
        if (bindings == NULL) {
            continue;
        }
        for (uint32_t k = 0; k < KEYS; k++) {
            if (!mask_get(live_set_mask[l], k)) {
                continue;
            }
            uint32_t dts_color = bindings[k].param1;
            if (live_colors[l][k] != dts_color) {
                LOG_DBG("rgb_studio: clearing stale live overlay layer=%d key=%u "
                        "(live=0x%08x dts=0x%08x)",
                        l, k, live_colors[l][k], dts_color);
                live_colors[l][k] = 0;
                mask_clear_one(live_set_mask[l], k);
                dirty = true;
            }
        }
    }
    if (dirty) {
        int rc = settings_save_one("rgb/colors", live_colors, sizeof(live_colors));
        if (!rc) {
            rc = settings_save_one("rgb/mask", live_set_mask, sizeof(live_set_mask));
        }
        if (rc) {
            LOG_ERR("rgb_studio: failed to persist reconciled overlay (%d)", rc);
        } else {
            LOG_INF("rgb_studio: reconciled stale live overlay against DT bindings");
        }
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(rgb_studio, "rgb", NULL, rgb_settings_set, rgb_settings_commit,
                               NULL);
