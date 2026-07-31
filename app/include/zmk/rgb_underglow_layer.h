/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <zmk/keymap.h>

/* Live per-key colour overlay edited via the rgb Studio subsystem. Values are
 * packed 0xEERRGGBB. layer_id is the Studio-side persistent layer id. */
int zmk_rgb_underglow_layer_stage_set(uint32_t layer_id, uint32_t key_pos, uint32_t color);
int zmk_rgb_underglow_layer_set_transparent(uint32_t layer_id, bool transparent);
int zmk_rgb_underglow_layer_get_color(uint32_t layer_id, uint32_t key_pos, uint32_t *out);
bool zmk_rgb_underglow_layer_is_transparent(uint32_t layer_id);
/* Drop BOTH the committed and the staged overrides for a layer — "reset this
 * layer to its DT colours", which is what the Studio clear-layer RPC means. */
void zmk_rgb_underglow_layer_clear(uint32_t layer_id);
/* Drop only the STAGED overrides, leaving anything the user committed with
 * "Save changes" intact. Transient painters (the games runtime) stage without
 * ever saving, so this is how they release the layer: clearing the committed
 * overlay too would silently discard the user's saved per-key colours for that
 * layer until the next reboot reloaded them from NVS. */
void zmk_rgb_underglow_layer_clear_pending(uint32_t layer_id);
/* Stage EVERY key of a layer to one colour in a single call — the bulk
 * form of stage_set. Exists so a transient painter can establish a
 * uniform canvas without 80 individual writes; over the split link the
 * matching SET_RGB_FILL command makes the peripheral do this loop
 * locally, so the whole canvas costs one radio packet instead of 80. */
void zmk_rgb_underglow_layer_fill(uint32_t layer_id, uint32_t color);
int zmk_rgb_underglow_layer_save(void);
void zmk_rgb_underglow_layer_discard(void);
void zmk_rgb_underglow_layer_reset_all(void);

/* Render-path hooks (called from rgb_underglow.c). layer_index is a runtime
 * keymap layer index, *not* the persistent layer_id. */
bool zmk_rgb_underglow_studio_lookup(uint8_t layer_index, uint32_t key_pos, uint32_t *out);
bool zmk_rgb_underglow_studio_layer_transparent(uint8_t layer_index);

#define ZMK_RGB_CHILD_LEN_PLUS_ONE(node) 1 +

#define ZMK_RGBMAP_LAYERS_LEN                                                                      \
    (DT_FOREACH_CHILD(DT_INST(0, zmk_underglow_layer), ZMK_RGB_CHILD_LEN_PLUS_ONE) 0)

#define ZMK_RGBMAP_EXTRACT_BINDING(idx, drv_inst)                                                  \
    {                                                                                              \
        .behavior_dev = DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(drv_inst, bindings, idx)),                \
        .param1 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(drv_inst, bindings, idx, param1), (0),        \
                              (DT_PHA_BY_IDX(drv_inst, bindings, idx, param1))),                   \
        .param2 = COND_CODE_0(DT_PHA_HAS_CELL_AT_IDX(drv_inst, bindings, idx, param2), (0),        \
                              (DT_PHA_BY_IDX(drv_inst, bindings, idx, param2))),                   \
    }

const int rgb_pixel_lookup(int idx);
const int zmk_rgbmap_id(uint8_t layer);
const int zmk_rgbmap_fade_delay(uint8_t layer);

const struct zmk_behavior_binding *rgb_underglow_get_bindings(uint8_t layer);

uint8_t rgb_underglow_top_layer_with_state(uint32_t state_to_test);
uint8_t rgb_underglow_top_layer(void);
uint32_t rgb_underglow_layers_state(void);
