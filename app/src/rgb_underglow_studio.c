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
#include <zephyr/sys/crc.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/behavior.h>
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

/* DTS-binding fingerprint used to detect "the per-key DT colours have
 * changed since the last save, the NVS overlay is stale". CRC32 of the
 * concatenated param1 fields of every DT-baked binding (which is the
 * 0xEERRGGBB encoding the editor's codegen emits). On boot we compute
 * the firmware's compiled-in fingerprint, compare to the one persisted
 * in NVS the last time the wipe ran. Match → DT colours unchanged, the
 * live overlay is authoritative across the reboot (Studio "Save changes"
 * mid-session survives power-cycle, AND firmware flashes that didn't
 * touch per-key colours preserve user commits). Mismatch → DT colours
 * changed, NVS overlay is stale relative to the new bindings → wipe.
 *
 * Why CRC of bindings, not __DATE__/__TIME__: ccache keys object files
 * by source hash + flags, stripping the time macros before hashing. Two
 * compiles of the same source produce the same cached object → same
 * timestamp baked in. So a build that only changed keymap.dts (NOT
 * rgb_underglow_studio.c) would hit ccache and reuse the old timestamp
 * — sentinel matches, no wipe, the per-key bug we're fixing recurs.
 * Fingerprinting the actual DT data sidesteps the build-system layer
 * entirely. */
static uint32_t loaded_dts_crc;
static bool dts_crc_loaded;

static inline bool mask_get(const uint8_t *mask, uint32_t key) {
    return (mask[key / 8] >> (key % 8)) & 1u;
}
static inline void mask_set(uint8_t *mask, uint32_t key) {
    mask[key / 8] |= (uint8_t)(1u << (key % 8));
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

void zmk_rgb_underglow_layer_clear_pending(uint32_t layer_id) {
    int idx = layer_id_to_index(layer_id);
    if (idx < 0)
        return;
    mask_clear_all(pending_set_mask[idx]);
}

void zmk_rgb_underglow_layer_fill(uint32_t layer_id, uint32_t color) {
    int idx = layer_id_to_index(layer_id);
    if (idx < 0)
        return;
    for (uint32_t k = 0; k < KEYS; k++) {
        pending_colors[idx][k] = color;
        mask_set(pending_set_mask[idx], k);
    }
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
    if (settings_name_steq(name, "dts_crc", &next) && !next) {
        if (len != sizeof(loaded_dts_crc)) {
            /* Wrong-sized record → treat as missing; commit will wipe. */
            return -EINVAL;
        }
        if (read_cb(cb_arg, &loaded_dts_crc, sizeof(loaded_dts_crc)) < 0) {
            return -EINVAL;
        }
        dts_crc_loaded = true;
        return 0;
    }
    return 0;
}

/* Walk the DT-baked rgbmap and CRC the param1 of every binding. Result
 * is stable across reboots of the same firmware build and changes
 * whenever the editor's codegen emits a different per-key colour map.
 * Cheap: single linear pass, ~LAYERS × KEYS × 4 bytes hashed.
 *
 * Gated on CONFIG_EXPERIMENTAL_RGB_LAYER because rgb_underglow_get_bindings
 * is only LINKED when that feature is on (rgb_underglow_layer.c is gated
 * by `target_sources_ifdef(CONFIG_EXPERIMENTAL_RGB_LAYER ...)` in
 * app/CMakeLists.txt). When the feature is off there are no DT-baked
 * per-key bindings to fingerprint, and the renderer never consults
 * studio_lookup, so returning 0 is the right stub: the commit handler
 * sees no per-key state to reconcile and stays a no-op. Without this
 * gate the prebuild step in the docker builder Dockerfile fails to
 * link with `undefined reference to rgb_underglow_get_bindings`,
 * silently leaving the docker image stuck at the previously-cached
 * build — every workspace UF2 produced afterwards lacks the per-key
 * NVS reconcile we're trying to ship. */
static uint32_t compute_dts_crc(void) {
    uint32_t crc = 0;
#if IS_ENABLED(CONFIG_EXPERIMENTAL_RGB_LAYER)
    int layers_with = 0, layers_without = 0;
    for (int l = 0; l < LAYERS; l++) {
        const struct zmk_behavior_binding *bindings = rgb_underglow_get_bindings((uint8_t)l);
        if (bindings == NULL) {
            layers_without++;
            continue;
        }
        layers_with++;
        for (uint32_t k = 0; k < KEYS; k++) {
            uint32_t v = bindings[k].param1;
            crc = crc32_ieee_update(crc, (const uint8_t *)&v, sizeof(v));
        }
    }
    LOG_INF("rgb_studio: compute_dts_crc → 0x%08x (layers_with=%d layers_without=%d, LAYERS=%d "
            "KEYS=%d)",
            crc, layers_with, layers_without, (int)LAYERS, (int)KEYS);
#endif
    return crc;
}

/* h_commit fires after settings_load has filled live_colors /
 * live_set_mask / layer_transparent / loaded_dts_crc from NVS. Compute
 * the firmware's DT-binding fingerprint (CRC of param1 across every
 * baked-in binding); compare to the NVS-stored fingerprint from the
 * last wipe. Match → DT colours unchanged, live overlay is
 * authoritative across the reboot (Studio "Save changes" mid-session
 * survives power-cycle, firmware flashes that touched non-RGB code
 * also preserve user commits). Mismatch (or no record) → DT colours
 * changed since the last save → wipe.
 *
 * Recovery semantics: on power loss mid-wipe, the dts_crc record is
 * persisted LAST. If any earlier save fails or power drops before
 * dts_crc lands, the next boot sees the same mismatch and re-runs the
 * wipe. Idempotent.
 *
 * Scope: ONLY touches the rgb/ subtree. BLE bonds, keymap layer
 * names, ext_power, backlight, behavior settings — all preserved
 * across the wipe. */
static int rgb_settings_commit(void) {
    uint32_t expected_crc = compute_dts_crc();
    LOG_INF("rgb_studio: commit fired (loaded_crc_present=%d loaded=0x%08x expected=0x%08x)",
            (int)dts_crc_loaded, loaded_dts_crc, expected_crc);
    if (dts_crc_loaded && loaded_dts_crc == expected_crc) {
        LOG_INF("rgb_studio: CRC match — keeping live overlay");
        return 0;
    }
    LOG_INF("rgb_studio: CRC mismatch — wiping per-key NVS overlay");
    memset(live_colors, 0, sizeof(live_colors));
    memset(live_set_mask, 0, sizeof(live_set_mask));
    memset(layer_transparent, 0, sizeof(layer_transparent));

    int rc = settings_save_one("rgb/colors", live_colors, sizeof(live_colors));
    if (!rc) {
        rc = settings_save_one("rgb/mask", live_set_mask, sizeof(live_set_mask));
    }
    if (!rc) {
        rc = settings_save_one("rgb/trans", layer_transparent, sizeof(layer_transparent));
    }
    if (!rc) {
        /* Persist the fingerprint LAST so a power-loss mid-wipe leaves
         * the system biased toward "still need to wipe" rather than
         * "wipe complete but colours not yet cleared". */
        rc = settings_save_one("rgb/dts_crc", &expected_crc, sizeof(expected_crc));
    }
    if (rc) {
        LOG_ERR("rgb_studio: failed to persist wiped overlay (%d)", rc);
    } else {
        loaded_dts_crc = expected_crc;
        dts_crc_loaded = true;
        LOG_INF("rgb_studio: per-key NVS overlay wiped, fingerprint=0x%08x", expected_crc);
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(rgb_studio, "rgb", NULL, rgb_settings_set, rgb_settings_commit,
                               NULL);
