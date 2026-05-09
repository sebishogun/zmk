/*
 * AuroraKey idle dimmer public surface — exposed only so rgb_underglow.c
 * can re-anchor the dimmer's "active target" when the user changes
 * brightness through any non-dimmer path (Studio save, &rgb_ug RGB_BRI,
 * &rgb_ug RGB_BRD, manual config bump). Without this hook the dimmer's
 * cached active_target_pct goes stale the moment the user changes
 * brightness, and the next wake-from-idle ramps back to the OLD value,
 * silently overwriting the user's new setting.
 *
 * Compiled to nothing on peripheral halves (the dimmer itself is gated
 * to ZMK_SPLIT_ROLE_CENTRAL) — the notify call sites are gated the
 * same way at use site.
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_IDLE_DIMMER)

/* Tell the idle dimmer that the user just changed brightness to
 * `new_pct` (0..100). The dimmer re-anchors its active target so the
 * next wake-from-idle ramps back to this value instead of the boot-
 * time default. No-op when the dimmer is mid-ramp — the in-flight
 * ramp keeps writing toward its existing target; the next IDLE event
 * re-anchors from the post-ramp baseline. */
void zmk_rgb_idle_dimmer_notify_user_brightness(uint8_t new_pct);

/* Returns the user's intended ("active") brightness as anchored by
 * the dimmer, or 0 if the dimmer hasn't anchored yet (boot before
 * first IDLE). Used by zmk_rgb_underglow_save_state to persist the
 * user's pre-dim brightness to NVS instead of the dimmer's transient
 * floor. Without this, save_state firing while the dimmer has ramped
 * state.color.b down to FLOOR_PCT would write the floor value to
 * flash, and the next cold boot (from deep sleep) would load it back
 * as the "saved" brightness — keyboard wakes stuck dim. */
uint8_t zmk_rgb_idle_dimmer_get_user_brightness_or_zero(void);

#else /* !CONFIG_ZMK_STUDIO_IDLE_DIMMER */

static inline void zmk_rgb_idle_dimmer_notify_user_brightness(uint8_t new_pct) { (void)new_pct; }
static inline uint8_t zmk_rgb_idle_dimmer_get_user_brightness_or_zero(void) { return 0; }

#endif /* CONFIG_ZMK_STUDIO_IDLE_DIMMER */
