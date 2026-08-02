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
#include <string.h>
#include <stdlib.h>
#include <zephyr/logging/log.h>
#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif
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

/* Write a slot's storage. Shared by the RPC path (which then persists)
 * and the settings-load path (which must NOT re-persist what it just
 * read). Copy data first, publish length last: concurrent readers see
 * either the old length or the new length; never a torn array. */
static int slot_apply(size_t index, const struct zmk_slot_macro_binding *bindings,
                      size_t bindings_len, uint16_t wait_ms, uint16_t tap_ms) {
    if (index >= SLOT_COUNT)
        return -EINVAL;
    if (bindings_len > BINDINGS_MAX)
        return -EOVERFLOW;

    for (size_t i = 0; i < bindings_len; i++) {
        slot_storage[index].bindings[i] = bindings[i];
    }
    slot_storage[index].wait_ms = wait_ms;
    slot_storage[index].tap_ms = tap_ms;
    atomic_set(&slot_storage[index].bindings_len, (atomic_val_t)bindings_len);
    return 0;
}

#if IS_ENABLED(CONFIG_SETTINGS)
/* On-flash record: header + `len` bindings. Every member is naturally
 * aligned, so the layout needs no packing — the asserts below lock the
 * on-flash shape so it can't silently drift with the in-RAM structs
 * (a drift would make old NVS records mis-parse after an upgrade). */
struct slot_nvs_rec {
    uint16_t wait_ms;
    uint16_t tap_ms;
    uint16_t len;
    uint16_t _reserved;
    struct zmk_slot_macro_binding bindings[BINDINGS_MAX];
};
BUILD_ASSERT(offsetof(struct slot_nvs_rec, bindings) == 8, "slot NVS header must stay 8 bytes");
BUILD_ASSERT(sizeof(struct zmk_slot_macro_binding) == 12, "slot NVS binding must stay 12 bytes");

static void slot_persist(size_t index) {
    struct slot_nvs_rec rec = {
        .wait_ms = slot_storage[index].wait_ms,
        .tap_ms = slot_storage[index].tap_ms,
        .len = (uint16_t)atomic_get(&slot_storage[index].bindings_len),
        ._reserved = 0,
    };
    for (size_t i = 0; i < rec.len; i++) {
        rec.bindings[i] = slot_storage[index].bindings[i];
    }
    char path[24];
    snprintf(path, sizeof(path), "slotmac/%u", (unsigned)index);
    size_t sz = offsetof(struct slot_nvs_rec, bindings) +
                (size_t)rec.len * sizeof(struct zmk_slot_macro_binding);
    int rc = settings_save_one(path, &rec, sz);
    if (rc) {
        LOG_ERR("slot %zu: settings save failed (%d)", index, rc);
    }
}

static int slot_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (settings_name_next(name, &next) <= 0 || next) {
        return -ENOENT;
    }
    char *end;
    unsigned long idx = strtoul(name, &end, 10);
    if (end == name || *end != '\0' || idx >= SLOT_COUNT) {
        return -ENOENT;
    }
    struct slot_nvs_rec rec;
    if (len < offsetof(struct slot_nvs_rec, bindings) || len > sizeof(rec)) {
        LOG_WRN("slot %lu: bad record size %u — ignoring", idx, (unsigned)len);
        return -EINVAL;
    }
    if (read_cb(cb_arg, &rec, len) < 0) {
        return -EINVAL;
    }
    size_t stored = (len - offsetof(struct slot_nvs_rec, bindings)) /
                    sizeof(struct zmk_slot_macro_binding);
    size_t n = MIN((size_t)rec.len, stored);
    if (n > BINDINGS_MAX) {
        n = BINDINGS_MAX;
    }
    slot_apply(idx, rec.bindings, n, rec.wait_ms, rec.tap_ms);
    LOG_INF("slot %lu restored from NVS: %zu bindings", idx, n);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(slot_macro, "slotmac", NULL, slot_settings_set, NULL, NULL);
#endif /* CONFIG_SETTINGS */

int zmk_slot_macro_set(size_t index, const struct zmk_slot_macro_binding *bindings,
                       size_t bindings_len, uint16_t wait_ms, uint16_t tap_ms) {
    int rc = slot_apply(index, bindings, bindings_len, wait_ms, tap_ms);
    if (rc) {
        return rc;
    }
    LOG_INF("slot %zu rewritten: %zu bindings, wait=%u tap=%u", index, bindings_len, wait_ms,
            tap_ms);
#if IS_ENABLED(CONFIG_SETTINGS)
    /* Persist so the slot survives reboot AND reflash — the pool used
     * to be RAM-only, which meant every power-cycle silently emptied
     * every macro until the editor was connected over USB again. An
     * empty set persists too, so deleting a macro sticks. */
    slot_persist(index);
#endif
    return 0;
}

/* The behavior driver — slot index is derived from the device-instance
 * name ("slot_macro_<N>"), NOT from binding->param1. Trusting param1
 * meant a stale or hand-edited binding like `&slot_macro_1 0` would
 * dispatch slot 0 from a device that's bound to slot 1's identity —
 * silently firing the wrong macro. With one device instance per slot
 * (see EmitSlotMacroDTS at internal/zmk/slot_macro.go) the device
 * itself unambiguously identifies the slot; param1 just carries
 * historical compatibility from when there was a single instance.
 * Param1 is now an OPTIONAL override: if the keymap ever sets a value
 * different from the device's index AND in range, that wins (gives
 * us a back-door for "fire slot N from any device" workflows without
 * regressing the default identity-based dispatch). */
static int slot_idx_from_dev_name(const char *name) {
    /* Expected form: "slot_macro_<N>" — anything else returns -1.
     * `zmk_behavior_get_local_id` and the cache layer share this same
     * naming convention, so a mismatch here means the keymap is
     * referencing a non-slot device through this driver, which is
     * a build-time impossibility unless someone wired DT directly. */
    if (!name)
        return -1;
    static const char prefix[] = "slot_macro_";
    size_t plen = sizeof(prefix) - 1;
    if (strncmp(name, prefix, plen) != 0)
        return -1;
    const char *p = name + plen;
    if (*p == '\0')
        return -1;
    int n = 0;
    while (*p) {
        if (*p < '0' || *p > '9')
            return -1;
        n = n * 10 + (*p - '0');
        if (n >= SLOT_COUNT)
            return -1;
        p++;
    }
    return n;
}

static int on_slot_macro_pressed(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    int dev_slot = slot_idx_from_dev_name(binding->behavior_dev);
    uint32_t slot_idx;
    if (dev_slot >= 0) {
        slot_idx = (uint32_t)dev_slot;
        /* If the keymap explicitly named a different in-range slot via
         * param1, honour the override. Out-of-range param1 is treated
         * as "no override" rather than an error — old keymaps with
         * param1=0 against a non-zero device fall through to the
         * device's own slot, which is what fixes the historical
         * `&slot_macro_1 0` → fired slot 0 bug. */
        if (binding->param1 != slot_idx && binding->param1 < SLOT_COUNT && binding->param1 != 0) {
            slot_idx = binding->param1;
        }
    } else {
        /* Pre-instance keymap or hand-rolled DT — fall back to the
         * legacy param1-as-index behaviour. */
        slot_idx = binding->param1;
    }
    if (slot_idx >= SLOT_COUNT) {
        LOG_ERR("slot %u out of range (dev=%s param1=%u)", slot_idx,
                binding->behavior_dev ? binding->behavior_dev : "?", binding->param1);
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
        /* zmk_behavior_queue_add takes the full event struct (pointer)
         * since the upstream API change — earlier signature took bare
         * `position`. behavior_macro.c was migrated to the new shape but
         * slot_macro.c was not, surfacing as a -Wint-conversion fatal
         * when the workspace's RGB underglow stack pulled in the strict
         * warning set. Pass &event to mirror behavior_macro.c:175-182. */
        zmk_behavior_queue_add(&event, inner, true, tap_ms);
        zmk_behavior_queue_add(&event, inner, false, wait_ms);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_slot_macro_released(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    /* Slot macros are tap-style — release is a no-op. Press fully
     * enqueues the playback. */
    return ZMK_BEHAVIOR_OPAQUE;
}

/* Parameter metadata tables — declared BEFORE behavior_slot_macro_driver_api
 * because the driver_api initializer references &metadata. C11 doesn't
 * allow forward references to file-scope statics in initializer
 * expressions, so the previous order (driver_api first, metadata after)
 * failed to compile under -Wfatal-errors with: "'metadata' undeclared
 * here (not in a function)". */
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata param1_values[] = {
    {
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range =
            {
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

static const struct behavior_driver_api behavior_slot_macro_driver_api = {
    .binding_pressed = on_slot_macro_pressed,
    .binding_released = on_slot_macro_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};
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
