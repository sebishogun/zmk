/*
 * AuroraKey live-USB macro slot pool.
 *
 * Reserves CONFIG_ZMK_STUDIO_MACRO_SLOT_COUNT instances of a custom
 * `zmk,behavior-slot-macro` at compile time. The editor pushes binding
 * lists into individual slots over RPC; each slot stores its bindings
 * in a writable RAM buffer that the slot-macro driver evaluates the
 * same way `behavior_macro.c` evaluates a static macro.
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <pb_encode.h>
#include <pb_decode.h>
#include <zephyr/device.h>
#include <zmk/behavior.h>
#include <zmk/studio/rpc.h>
#include <zmk/behavior_slot_macro.h>

#define BEHAVIOR_RESPONSE(type, ...) ZMK_RPC_RESPONSE(behaviors, type, __VA_ARGS__)

/* Gate on the DT compat as well as the Kconfig: the slot behavior driver
 * only compiles when the keymap actually declares slot_macro_<N> nodes
 * (the editor emits Kconfig + DT together), so a conf that flips the
 * Kconfig on without the nodes must compile this subsystem out too or
 * the build dies at link on zmk_slot_macro_get/_set. */
#if IS_ENABLED(CONFIG_ZMK_STUDIO_MACRO_SLOT_POOL) &&                                               \
    DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_slot_macro)

#define SLOT_COUNT CONFIG_ZMK_STUDIO_MACRO_SLOT_COUNT
#define BINDINGS_MAX CONFIG_ZMK_STUDIO_MACRO_SLOT_BINDINGS_MAX

/* Encoder for repeated MacroSlot — emits the SLOT_COUNT entries we
 * reserved at boot, even those still-empty. The editor uses
 * total_slots from the response to display "slot 3 / 8 used", so we
 * never lie about how many slots exist. */
struct slots_state {
    size_t i;
};

static bool encode_macro_slots(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    for (size_t i = 0; i < SLOT_COUNT; i++) {
        if (!pb_encode_tag_for_field(stream, field))
            return false;
        zmk_behaviors_MacroSlot slot = zmk_behaviors_MacroSlot_init_zero;
        slot.index = i;
        const struct zmk_slot_macro_state *st = zmk_slot_macro_get(i);
        slot.wait_ms = st->wait_ms;
        slot.tap_ms = st->tap_ms;
        /* label = "slot_N" — fixed pattern; editor can show a friendly
         * label locally without needing firmware roundtrip. */
        char label[12];
        snprintf(label, sizeof(label), "slot_%zu", i);
        strncpy(slot.label, label, sizeof(slot.label) - 1);
        size_t n = MIN(st->bindings_len, BINDINGS_MAX);
        for (size_t b = 0; b < n; b++) {
            slot.bindings[b].behavior_id = st->bindings[b].behavior_id;
            slot.bindings[b].param1 = st->bindings[b].param1;
            slot.bindings[b].param2 = st->bindings[b].param2;
        }
        slot.bindings_count = n;
        if (!pb_encode_submessage(stream, zmk_behaviors_MacroSlot_fields, &slot)) {
            return false;
        }
    }
    return true;
}

zmk_studio_Response list_macro_slots(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_behaviors_ListMacroSlotsResponse resp = zmk_behaviors_ListMacroSlotsResponse_init_zero;
    resp.slots.funcs.encode = encode_macro_slots;
    resp.total_slots = SLOT_COUNT;
    return BEHAVIOR_RESPONSE(list_macro_slots, resp);
}

zmk_studio_Response set_macro_slot(const zmk_studio_Request *req) {
    const zmk_behaviors_SetMacroSlotRequest *r =
        &req->subsystem.behaviors.request_type.set_macro_slot;
    LOG_INF("set_macro_slot idx=%u bindings_count=%u wait=%u tap=%u", r->index, r->bindings_count,
            r->wait_ms, r->tap_ms);

    zmk_behaviors_SetMacroSlotResponse resp = zmk_behaviors_SetMacroSlotResponse_init_zero;

    if (r->index >= SLOT_COUNT) {
        LOG_WRN("set_macro_slot: index %u >= SLOT_COUNT=%u", r->index, SLOT_COUNT);
        resp.result = zmk_behaviors_MacroSlotErrorCode_MACRO_SLOT_ERR_INVALID_INDEX;
        return BEHAVIOR_RESPONSE(set_macro_slot, resp);
    }
    if (r->bindings_count > BINDINGS_MAX) {
        LOG_WRN("set_macro_slot: bindings_count %u > BINDINGS_MAX=%u", r->bindings_count,
                BINDINGS_MAX);
        resp.result = zmk_behaviors_MacroSlotErrorCode_MACRO_SLOT_ERR_TOO_MANY_BINDINGS;
        return BEHAVIOR_RESPONSE(set_macro_slot, resp);
    }
    /* Validate every inner behavior_id resolves before mutating. The
     * proto's behavior_id is the studio local id (uint32) — the same
     * space `zmk_behavior_get_local_id(name)` returns. Resolve via
     * `zmk_behavior_find_behavior_name_from_local_id` (string) and
     * then look up the device. Calling `zmk_behavior_get_binding` with
     * the raw uint32 was the original bug: it expects a `const char *`
     * (behavior name), so the int got reinterpreted as a pointer →
     * compile warning + runtime garbage-deref → handler hung → editor
     * RPC timeout. */
    for (size_t i = 0; i < r->bindings_count; i++) {
        const char *behavior_name =
            zmk_behavior_find_behavior_name_from_local_id(r->bindings[i].behavior_id);
        if (!behavior_name || !zmk_behavior_get_binding(behavior_name)) {
            LOG_WRN("set_macro_slot: binding[%zu].behavior_id=%u doesn't resolve", i,
                    r->bindings[i].behavior_id);
            resp.result = zmk_behaviors_MacroSlotErrorCode_MACRO_SLOT_ERR_INVALID_INNER_BEHAVIOR;
            return BEHAVIOR_RESPONSE(set_macro_slot, resp);
        }
    }

    struct zmk_slot_macro_binding new_bindings[BINDINGS_MAX];
    for (size_t i = 0; i < r->bindings_count; i++) {
        new_bindings[i].behavior_id = r->bindings[i].behavior_id;
        new_bindings[i].param1 = r->bindings[i].param1;
        new_bindings[i].param2 = r->bindings[i].param2;
    }
    int rc = zmk_slot_macro_set(r->index, new_bindings, r->bindings_count, r->wait_ms, r->tap_ms);
    LOG_INF("set_macro_slot idx=%u rc=%d -> %s", r->index, rc, rc == 0 ? "OK" : "GENERIC");
    resp.result = (rc == 0) ? zmk_behaviors_MacroSlotErrorCode_MACRO_SLOT_ERR_OK
                            : zmk_behaviors_MacroSlotErrorCode_MACRO_SLOT_ERR_GENERIC;
    return BEHAVIOR_RESPONSE(set_macro_slot, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, list_macro_slots, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, set_macro_slot, ZMK_STUDIO_RPC_HANDLER_SECURED);

#endif /* CONFIG_ZMK_STUDIO_MACRO_SLOT_POOL */
