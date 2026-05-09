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

#else /* !CONFIG_ZMK_STUDIO_IDLE_DIMMER */

static inline void zmk_rgb_idle_dimmer_notify_user_brightness(uint8_t new_pct) {
    (void)new_pct;
}

#endif /* CONFIG_ZMK_STUDIO_IDLE_DIMMER */
