/*
 * AuroraKey idle-driven RGB brightness ramp.
 *
 * Smoothly ramps the underglow brightness to CONFIG_ZMK_STUDIO_IDLE_DIMMER_FLOOR_PCT
 * once the keyboard hits idle, and back up on the next activity event.
 * Fixes the harsh on/off flash of the stock CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE
 * — that one cuts power outright; this one tapers via the existing
 * brightness API.
 *
 * Hooked off two ZMK events (no new subsystems, no new threads):
 *   - zmk_activity_state_changed:  IDLE → schedule ramp-down
 *                                  ACTIVE → schedule ramp-up
 *   - the work_delayable cycle handles intermediate steps
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(idle_dimmer, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/rgb_underglow.h>
#include <zmk/activity.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_IDLE_DIMMER)

#define FLOOR_PCT     CONFIG_ZMK_STUDIO_IDLE_DIMMER_FLOOR_PCT
#define RAMP_MS       CONFIG_ZMK_STUDIO_IDLE_DIMMER_RAMP_MS
/* 16 steps gives a perceptually-smooth taper for the typical 800 ms
 * ramp window. Fewer steps and the eye sees discrete pops; more and
 * we burn idle CPU for sub-perceptual deltas. */
#define STEPS         16
#define STEP_INTERVAL (RAMP_MS / STEPS)

/* Brightness anchor — captured at boot from the live config so the
 * "active" target tracks whatever the user actually chose, not a
 * hardcoded default. Refreshed each time we leave idle so a brightness
 * change *during* idle is honoured on wake. */
static uint8_t active_target_pct = 0;
static uint8_t current_pct = 0;
static enum {
    DIRECTION_NONE,
    DIRECTION_DOWN,
    DIRECTION_UP,
} ramp_direction = DIRECTION_NONE;

static void ramp_step(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(ramp_work, ramp_step);

static void ramp_step(struct k_work *work) {
    ARG_UNUSED(work);
    /* Compute the delta toward the current target. Floor-clamping
     * is what stops the ramp once we hit either bound. */
    uint8_t target = (ramp_direction == DIRECTION_DOWN) ? FLOOR_PCT : active_target_pct;
    if (current_pct == target) {
        ramp_direction = DIRECTION_NONE;
        return;
    }
    /* Ceil-divided step size — guarantees we *reach* the target
     * exactly at STEPS iterations even when (target - current) doesn't
     * divide evenly, instead of stalling one short. */
    int diff = (int)target - (int)current_pct;
    int abs_diff = diff > 0 ? diff : -diff;
    int step = (abs_diff + STEPS - 1) / STEPS;
    if (step < 1) step = 1;
    if (diff < 0) step = -step;

    int next = (int)current_pct + step;
    /* Clamp to target so the last step never overshoots. */
    if ((diff > 0 && next > target) || (diff < 0 && next < target)) {
        next = target;
    }
    current_pct = (uint8_t)next;
    zmk_rgb_underglow_set_brightness((uint8_t)current_pct);

    if (current_pct != target) {
        k_work_schedule(&ramp_work, K_MSEC(STEP_INTERVAL));
    } else {
        ramp_direction = DIRECTION_NONE;
    }
}

static int on_activity_state_changed(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev =
        as_zmk_activity_state_changed(eh);
    if (!ev) return ZMK_EV_EVENT_BUBBLE;

    switch (ev->state) {
    case ZMK_ACTIVITY_ACTIVE:
        if (ramp_direction == DIRECTION_NONE && current_pct == active_target_pct) {
            return ZMK_EV_EVENT_BUBBLE; /* nothing to do */
        }
        ramp_direction = DIRECTION_UP;
        k_work_schedule(&ramp_work, K_NO_WAIT);
        break;
    case ZMK_ACTIVITY_IDLE:
    case ZMK_ACTIVITY_SLEEP:
        /* Capture the active brightness so we know what to ramp back
         * to. Only re-cache when we're actually at the active target
         * — mid-ramp captures would freeze the target at an
         * intermediate value. */
        if (ramp_direction == DIRECTION_NONE) {
            uint8_t live = zmk_rgb_underglow_calc_effective_brightness();
            if (live > FLOOR_PCT) {
                active_target_pct = live;
                current_pct = live;
            }
        }
        ramp_direction = DIRECTION_DOWN;
        k_work_schedule(&ramp_work, K_NO_WAIT);
        break;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(idle_dimmer, on_activity_state_changed);
ZMK_SUBSCRIPTION(idle_dimmer, zmk_activity_state_changed);

static int idle_dimmer_init(void) {
    /* Seed both anchors from the live config. ZMK boots with idle
     * state ACTIVE, so we never enter ramp_step before this callback
     * has run. */
    uint8_t boot = zmk_rgb_underglow_calc_effective_brightness();
    active_target_pct = boot;
    current_pct = boot;
    return 0;
}
SYS_INIT(idle_dimmer_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_ZMK_STUDIO_IDLE_DIMMER */
