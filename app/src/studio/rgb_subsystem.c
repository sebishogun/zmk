/*
 * AuroraKey per-key RGB Studio subsystem.
 *
 * Live-edit per-key colors over USB/BLE via the ZMK Studio RPC framework.
 * Mirrors the keymap subsystem's pending-changes flow: set_key_color stages a
 * change, save_changes commits to settings storage and reapplies the overlay,
 * discard_changes rolls back to the persisted state.
 *
 * Build prerequisites (see ../README.md):
 *   1. zmk-studio-messages fork includes rgb.proto and adds `zmk.rgb.Request rgb = 6;`
 *      to studio.proto Request / RequestResponse / Notification oneofs.
 *   2. west.yml in this fork pins the messages fork.
 *   3. CMakeLists.txt under app/src/studio/ adds rgb_subsystem.c to target_sources.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <pb_encode.h>
#include <zmk/matrix.h>
#include <zmk/studio/core.h>
#include <zmk/studio/rpc.h>
#include <zmk/rgb_underglow.h>
#include <zmk/rgb_underglow_layer.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

ZMK_RPC_SUBSYSTEM(rgb)

#define RGB_RESPONSE(type, ...) ZMK_RPC_RESPONSE(rgb, type, __VA_ARGS__)

static zmk_studio_Response set_key_color(const zmk_studio_Request *req) {
    const zmk_rgb_SetKeyColorRequest *r = &req->subsystem.rgb.request_type.set_key_color;
    int ret = zmk_rgb_underglow_layer_stage_set(r->layer_id, r->key_position, r->color);
    zmk_rgb_SetKeyColorResponse resp;
    switch (ret) {
    case 0:
        resp = zmk_rgb_SetKeyColorResponse_SET_KEY_COLOR_RESP_OK;
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && \
    IS_ENABLED(CONFIG_EXPERIMENTAL_RGB_LAYER)
        zmk_split_central_update_rgb_color(r->layer_id, r->key_position, r->color);
#endif
        break;
    case -EINVAL:
        resp = zmk_rgb_SetKeyColorResponse_SET_KEY_COLOR_RESP_INVALID_LAYER;
        break;
    case -ERANGE:
        resp = zmk_rgb_SetKeyColorResponse_SET_KEY_COLOR_RESP_INVALID_KEY;
        break;
    default:
        resp = zmk_rgb_SetKeyColorResponse_SET_KEY_COLOR_RESP_NO_SPACE;
        break;
    }
    return RGB_RESPONSE(set_key_color, resp);
}

static zmk_studio_Response set_layer_transparent(const zmk_studio_Request *req) {
    const zmk_rgb_SetLayerTransparentRequest *r =
        &req->subsystem.rgb.request_type.set_layer_transparent;
    int ret = zmk_rgb_underglow_layer_set_transparent(r->layer_id, r->transparent);
    zmk_rgb_SetLayerTransparentResponse resp =
        ret == 0 ? zmk_rgb_SetLayerTransparentResponse_SET_LAYER_TRANSPARENT_RESP_OK
                 : zmk_rgb_SetLayerTransparentResponse_SET_LAYER_TRANSPARENT_RESP_INVALID_LAYER;
    return RGB_RESPONSE(set_layer_transparent, resp);
}

static bool encode_layer_colors_keys(pb_ostream_t *stream, const pb_field_t *field,
                                     void *const *arg) {
    uint32_t layer_id = (uint32_t)(uintptr_t)*arg;
    for (uint32_t pos = 0; pos < ZMK_KEYMAP_LEN; pos++) {
        uint32_t color;
        if (zmk_rgb_underglow_layer_get_color(layer_id, pos, &color) != 0)
            continue;
        if (!pb_encode_tag_for_field(stream, field))
            return false;
        zmk_rgb_KeyColor entry = {.key_position = pos, .color = color};
        if (!pb_encode_submessage(stream, &zmk_rgb_KeyColor_msg, &entry))
            return false;
    }
    return true;
}

static zmk_studio_Response get_layer_colors(const zmk_studio_Request *req) {
    const zmk_rgb_GetLayerColorsRequest *r = &req->subsystem.rgb.request_type.get_layer_colors;
    zmk_rgb_LayerColors resp = zmk_rgb_LayerColors_init_zero;
    resp.layer_id = r->layer_id;
    resp.transparent = zmk_rgb_underglow_layer_is_transparent(r->layer_id);
    resp.keys.funcs.encode = encode_layer_colors_keys;
    resp.keys.arg = (void *)(uintptr_t)r->layer_id;
    return RGB_RESPONSE(get_layer_colors, resp);
}

static zmk_studio_Response clear_layer(const zmk_studio_Request *req) {
    uint32_t layer_id = req->subsystem.rgb.request_type.clear_layer.layer_id;
    zmk_rgb_underglow_layer_clear(layer_id);
    return RGB_RESPONSE(clear_layer, true);
}

static zmk_studio_Response save_changes(const zmk_studio_Request *req) {
    int ret = zmk_rgb_underglow_layer_save();
    return RGB_RESPONSE(save_changes, ret == 0);
}

static zmk_studio_Response discard_changes(const zmk_studio_Request *req) {
    zmk_rgb_underglow_layer_discard();
    return RGB_RESPONSE(discard_changes, true);
}

ZMK_RPC_SUBSYSTEM_HANDLER(rgb, set_key_color, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(rgb, set_layer_transparent, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(rgb, get_layer_colors, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(rgb, clear_layer, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(rgb, save_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(rgb, discard_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int rgb_subsystem_settings_reset(void) {
    zmk_rgb_underglow_layer_reset_all();
    return 0;
}

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(rgb, rgb_subsystem_settings_reset);
