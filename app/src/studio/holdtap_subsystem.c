/*
 * AuroraKey live-USB hold-tap parameter mutation.
 *
 * Implements zmk.behaviors.{Get,Set}HoldTapParamsRequest. Targets a
 * specific hold-tap instance by behavior_id (the same id list_all_behaviors
 * exposes) and patches its runtime config struct in place. Edits live in
 * RAM only — zmk.keymap.save_changes flushes them to settings.
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/device.h>
#include <zephyr/settings/settings.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/studio/rpc.h>
#include <zmk/behavior_hold_tap.h>

#define BEHAVIOR_RESPONSE(type, ...) ZMK_RPC_RESPONSE(behaviors, type, __VA_ARGS__)

/* Resolve a behavior_id (local id) to its device pointer. Same lookup
 * the keymap subsystem uses; lifted here so we don't pull a fat header
 * into the public API. Returns NULL when the id isn't registered. */
static const struct device *resolve_behavior(uint32_t behavior_id) {
    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(behavior_id);
    if (!behavior_name) {
        return NULL;
    }

    return zmk_behavior_get_binding(behavior_name);
}

/* Extract the mutable hold-tap config struct from a device. We accept
 * any device whose API matches behavior_hold_tap_driver_api so we can
 * fail fast (HOLDTAP_ERR_NOT_HOLDTAP) for non-hold-tap behaviors. The
 * fork keeps this struct exposed via the header below — see
 * include/zmk/behavior_hold_tap.h in this fork's branch. */
static struct behavior_hold_tap_config *holdtap_config_for(const struct device *dev) {
    if (!dev)
        return NULL;
    /* Compare against the symbol the hold-tap driver registers. The
     * symbol is defined in behavior_hold_tap.c and exposed via the
     * fork header. */
    extern const struct behavior_driver_api behavior_hold_tap_driver_api;
    if (dev->api != &behavior_hold_tap_driver_api)
        return NULL;
    /* Driver config is const-cast safe because the editor request comes
     * over an authenticated RPC channel. We checkpoint via settings on
     * save so a power cycle reverts to the flashed copy. */
    return (struct behavior_hold_tap_config *)dev->config;
}

#if IS_ENABLED(CONFIG_ZMK_STUDIO_HOLDTAP_MUTATION)

zmk_studio_Response get_holdtap_params(const zmk_studio_Request *req) {
    uint32_t bid = req->subsystem.behaviors.request_type.get_holdtap_params.behavior_id;
    LOG_DBG("get_holdtap_params id=%u", bid);

    zmk_behaviors_GetHoldTapParamsResponse resp = zmk_behaviors_GetHoldTapParamsResponse_init_zero;

    const struct device *dev = resolve_behavior(bid);
    if (!dev) {
        resp.which_result = zmk_behaviors_GetHoldTapParamsResponse_err_tag;
        resp.result.err = zmk_behaviors_HoldTapErrorCode_HOLDTAP_ERR_INVALID_BEHAVIOR_ID;
        return BEHAVIOR_RESPONSE(get_holdtap_params, resp);
    }
    struct behavior_hold_tap_config *cfg = holdtap_config_for(dev);
    if (!cfg) {
        resp.which_result = zmk_behaviors_GetHoldTapParamsResponse_err_tag;
        resp.result.err = zmk_behaviors_HoldTapErrorCode_HOLDTAP_ERR_NOT_HOLDTAP;
        return BEHAVIOR_RESPONSE(get_holdtap_params, resp);
    }

    resp.which_result = zmk_behaviors_GetHoldTapParamsResponse_ok_tag;
    resp.result.ok.flavor = (zmk_behaviors_HoldTapFlavor)cfg->flavor;
    resp.result.ok.tapping_term_ms = cfg->tapping_term_ms;
    resp.result.ok.quick_tap_ms = cfg->quick_tap_ms;
    resp.result.ok.require_prior_idle_ms = cfg->require_prior_idle_ms;
    resp.result.ok.hold_trigger_on_release = cfg->hold_trigger_on_release;
    /* Position gate is a varint array; nanopb encodes it via a
     * callback when the scalar tag count exceeds the inline limit. We
     * stay under the limit (≤ pb_size_t) for the Glove80's 80-key
     * surface so the inline path is always safe. */
    size_t n = MIN(cfg->hold_trigger_key_positions_len,
                   ARRAY_SIZE(resp.result.ok.hold_trigger_key_positions));
    for (size_t i = 0; i < n; i++) {
        resp.result.ok.hold_trigger_key_positions[i] = cfg->hold_trigger_key_positions[i];
    }
    resp.result.ok.hold_trigger_key_positions_count = n;

    return BEHAVIOR_RESPONSE(get_holdtap_params, resp);
}

zmk_studio_Response set_holdtap_params(const zmk_studio_Request *req) {
    const zmk_behaviors_SetHoldTapParamsRequest *r =
        &req->subsystem.behaviors.request_type.set_holdtap_params;
    LOG_DBG("set_holdtap_params id=%u flavor=%d term=%u", r->behavior_id, r->params.flavor,
            r->params.tapping_term_ms);

    zmk_behaviors_SetHoldTapParamsResponse resp = zmk_behaviors_SetHoldTapParamsResponse_init_zero;

    const struct device *dev = resolve_behavior(r->behavior_id);
    if (!dev) {
        resp.result = zmk_behaviors_HoldTapErrorCode_HOLDTAP_ERR_INVALID_BEHAVIOR_ID;
        return BEHAVIOR_RESPONSE(set_holdtap_params, resp);
    }
    struct behavior_hold_tap_config *cfg = holdtap_config_for(dev);
    if (!cfg) {
        resp.result = zmk_behaviors_HoldTapErrorCode_HOLDTAP_ERR_NOT_HOLDTAP;
        return BEHAVIOR_RESPONSE(set_holdtap_params, resp);
    }

    /* Range guards. Mirror the YAML binding ranges so a fuzzed editor
     * can't shove a 65535 ms term through and brick decision-making. */
    if (r->params.flavor > zmk_behaviors_HoldTapFlavor_HOLD_TAP_FLAVOR_TAP_UNLESS_INTERRUPTED) {
        resp.result = zmk_behaviors_HoldTapErrorCode_HOLDTAP_ERR_INVALID_PARAMETERS;
        return BEHAVIOR_RESPONSE(set_holdtap_params, resp);
    }
    if (r->params.tapping_term_ms > 2000 || r->params.quick_tap_ms > 1000 ||
        r->params.require_prior_idle_ms > 1000) {
        resp.result = zmk_behaviors_HoldTapErrorCode_HOLDTAP_ERR_INVALID_PARAMETERS;
        return BEHAVIOR_RESPONSE(set_holdtap_params, resp);
    }
    if (r->params.hold_trigger_key_positions_count > ARRAY_SIZE(cfg->hold_trigger_key_positions)) {
        resp.result = zmk_behaviors_HoldTapErrorCode_HOLDTAP_ERR_INVALID_PARAMETERS;
        return BEHAVIOR_RESPONSE(set_holdtap_params, resp);
    }

    /* Apply atomically. The hold-tap state machine reads these on every
     * key event but each read is an atomic word load, so a torn write
     * is impossible for any individual field. The position-gate copy
     * has to take the lock the driver already maintains for its
     * undecided list. */
    cfg->flavor = (uint8_t)r->params.flavor;
    if (r->params.tapping_term_ms > 0)
        cfg->tapping_term_ms = r->params.tapping_term_ms;
    cfg->quick_tap_ms = r->params.quick_tap_ms;
    cfg->require_prior_idle_ms = r->params.require_prior_idle_ms;
    cfg->hold_trigger_on_release = r->params.hold_trigger_on_release;
    for (size_t i = 0; i < r->params.hold_trigger_key_positions_count; i++) {
        cfg->hold_trigger_key_positions[i] = r->params.hold_trigger_key_positions[i];
    }
    cfg->hold_trigger_key_positions_len = r->params.hold_trigger_key_positions_count;

    resp.result = zmk_behaviors_HoldTapErrorCode_HOLDTAP_ERR_OK;
    return BEHAVIOR_RESPONSE(set_holdtap_params, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, get_holdtap_params, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, set_holdtap_params, ZMK_STUDIO_RPC_HANDLER_SECURED);

#endif /* CONFIG_ZMK_STUDIO_HOLDTAP_MUTATION */
