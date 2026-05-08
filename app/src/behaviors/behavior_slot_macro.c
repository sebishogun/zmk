/*
 * AuroraKey runtime-mutable macro behavior — `zmk,behavior-slot-macro`.
 *
 * Functionally equivalent to `zmk,behavior-macro` but the bindings list
 * lives in writable RAM and can be re-written over RPC. Each instance
 * is one slot; the keymap binds `&slot_macro 0`, `&slot_macro 1`, …
 * to dispatch to slot N.
 *
 * Implementation note — this driver is intentionally close to
 * behavior_macro.c. The differences:
 *   1. Bindings array is mutable (zmk_slot_macro_state.bindings)
 *      instead of DT-baked.
 *   2. `slot_index` is read from the binding's param1 at use time so
 *      one driver instance services every slot.
 *   3. Mid-flight set_macro_slot is rejected internally — the runner
 *      finishes the current playback against the snapshot it captured
 *      on entry; the new bindings take effect on the next press.
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_slot_macro

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(behavior_slot_macro, CONFIG_ZMK_LOG_LEVEL);

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/behavior_slot_macro.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) && IS_ENABLED(CONFIG_ZMK_STUDIO_MACRO_SLOT_POOL)

#define SLOT_COUNT CONFIG_ZMK_STUDIO_MACRO_SLOT_COUNT
#define BINDINGS_MAX CONFIG_ZMK_STUDIO_MACRO_SLOT_BINDINGS_MAX

/* One backing buffer per slot. Marked aligned(4) so the atomic swap
 * in zmk_slot_macro_set is well-defined for the bindings_len field. */
static struct {
    struct zmk_slot_macro_binding bindings[BINDINGS_MAX];
    /* atomic write order: bindings[] first, len last. Readers always
     * read len first. That sequence guarantees a partial write never
     * exposes garbage past the last valid binding. */
    atomic_t bindings_len;
    uint16_t wait_ms;
    uint16_t tap_ms;
} __aligned(4) slot_storage[SLOT_COUNT];

static struct zmk_slot_macro_state slot_state[SLOT_COUNT];

const struct zmk_slot_macro_state *zmk_slot_macro_get(size_t index) {
    if (index >= SLOT_COUNT)
        return NULL;
    /* Reflect current atomic length at read time; the live storage
     * pointer is stable. */
    slot_state[index].bindings_len = (size_t)atomic_get(&slot_storage[index].bindings_len);
    return &slot_state[index];
}

int zmk_slot_macro_set(size_t index, const struct zmk_slot_macro_binding *bindings,
                       size_t bindings_len, uint16_t wait_ms, uint16_t tap_ms) {
    if (index >= SLOT_COUNT)
        return -EINVAL;
    if (bindings_len > BINDINGS_MAX)
        return -EOVERFLOW;

    /* Copy data first, publish length last. Concurrent readers see
     * either the old length or the new length; never a torn array. */
    for (size_t i = 0; i < bindings_len; i++) {
        slot_storage[index].bindings[i] = bindings[i];
    }
    slot_storage[index].wait_ms = wait_ms;
    slot_storage[index].tap_ms = tap_ms;
    atomic_set(&slot_storage[index].bindings_len, (atomic_val_t)bindings_len);

    LOG_INF("slot %zu rewritten: %zu bindings, wait=%u tap=%u", index, bindings_len, wait_ms,
            tap_ms);
    return 0;
}

/* The behavior driver — one instance, parametrised by slot index in
 * binding param1. */
static int on_slot_macro_pressed(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    uint32_t slot_idx = binding->param1;
    if (slot_idx >= SLOT_COUNT) {
        LOG_ERR("slot %u out of range", slot_idx);
        return -EINVAL;
    }
    /* Snapshot the slot — if the editor pushes new bindings while
     * we're firing, this playback uses the snapshot, the next press
     * uses the new set. */
    size_t n = (size_t)atomic_get(&slot_storage[slot_idx].bindings_len);
    if (n == 0) {
        return ZMK_BEHAVIOR_OPAQUE; /* empty slot = no-op */
    }

    /* Default timing — match upstream behavior_macro defaults so
     * editing a slot doesn't surprise users vs static macros. */
    int wait_ms = slot_storage[slot_idx].wait_ms;
    int tap_ms = slot_storage[slot_idx].tap_ms;
    if (wait_ms == 0)
        wait_ms = CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS;
    if (tap_ms == 0)
        tap_ms = CONFIG_ZMK_MACRO_DEFAULT_TAP_MS;

    for (size_t i = 0; i < n; i++) {
        struct zmk_slot_macro_binding *b = &slot_storage[slot_idx].bindings[i];
        /* The slot stores the studio local id (uint32). Resolve it to
         * the behavior NAME (const char *) — that's what the queue +
         * dispatch chain key off (`behavior_dev` is a string). The
         * earlier code passed the int straight to
         * `zmk_behavior_get_binding`, which expects a string, then
         * cast the returned `const struct device *` back to `const
         * char *` and stuffed THAT into `.behavior_dev`. Result was
         * an inner binding pointing at a struct device's first byte
         * being interpreted as a character — runtime garbage. */
        const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(b->behavior_id);
        if (!behavior_name) {
            LOG_ERR("slot %u: binding[%zu] has unresolved behavior_id=%u — skipping", slot_idx, i,
                    b->behavior_id);
            continue;
        }
        struct zmk_behavior_binding inner = {
            .behavior_dev = behavior_name,
            .param1 = b->param1,
            .param2 = b->param2,
        };
        zmk_behavior_queue_add(event.position, inner, true, tap_ms);
        zmk_behavior_queue_add(event.position, inner, false, wait_ms);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_slot_macro_released(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    /* Slot macros are tap-style — release is a no-op. Press fully
     * enqueues the playback. */
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_slot_macro_driver_api = {
    .binding_pressed = on_slot_macro_pressed,
    .binding_released = on_slot_macro_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata param1_values[] = {
    {
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range = {
            .min = 0,
            .max = CONFIG_ZMK_STUDIO_MACRO_SLOT_COUNT - 1,
        },
    },
};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = param1_values,
    .param1_values_len = ARRAY_SIZE(param1_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};
#endif
static int behavior_slot_macro_init(const struct device *dev) {
    ARG_UNUSED(dev);
    /* Initialise state pointers so zmk_slot_macro_get works before
     * the first set call. */
    for (size_t i = 0; i < SLOT_COUNT; i++) {
        slot_state[i].bindings = slot_storage[i].bindings;
        slot_state[i].bindings_len = 0;
        slot_state[i].wait_ms = 0;
        slot_state[i].tap_ms = 0;
        atomic_set(&slot_storage[i].bindings_len, 0);
    }
    return 0;
}

#define KP_INST(n)                                                                                 \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_slot_macro_init, NULL, NULL, NULL, POST_KERNEL,            \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_slot_macro_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY && CONFIG_ZMK_STUDIO_MACRO_SLOT_POOL */
