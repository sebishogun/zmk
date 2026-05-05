/*
 * AuroraKey slot-macro public header.
 *
 * Slot macros are runtime-mutable equivalents of zmk,behavior-macro.
 * They live in a fixed pool (CONFIG_ZMK_STUDIO_MACRO_SLOT_COUNT) and
 * are referenced from the keymap by index — `&slot_macro N` invokes
 * slot N. Bindings get filled in at runtime over the studio RPC
 * (zmk.behaviors.set_macro_slot).
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* One step inside a slot. behavior_id matches the local id space the
 * studio's list_all_behaviors uses. param1/param2 are the standard
 * 2-cell behavior parameters (e.g. for `&kp 0x70004` param1=0x70004). */
struct zmk_slot_macro_binding {
    uint32_t behavior_id;
    uint32_t param1;
    uint32_t param2;
};

struct zmk_slot_macro_state {
    struct zmk_slot_macro_binding *bindings;
    size_t bindings_len;
    /* Optional timing overrides — 0 means "use the firmware default". */
    uint16_t wait_ms;
    uint16_t tap_ms;
};

/* Read-only accessor for the RPC encoder. Returns a pointer to the
 * slot's live state; never NULL when index < SLOT_COUNT (the boot
 * code reserves all slots up front). */
const struct zmk_slot_macro_state *zmk_slot_macro_get(size_t index);

/* Replace the bindings of slot `index`. Returns 0 on success, -EINVAL
 * for out-of-range index, -EOVERFLOW when bindings_len exceeds the
 * compile-time max. The caller validates inner behavior_ids; this
 * function is the trusted writer. Atomic w.r.t. concurrent macro
 * playback — if a slot is currently firing, the swap takes effect
 * for the *next* invocation, never mid-flight. */
int zmk_slot_macro_set(size_t index, const struct zmk_slot_macro_binding *bindings,
                       size_t bindings_len, uint16_t wait_ms, uint16_t tap_ms);
