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
 * Split-keyboard caveat: this file is gated on ZMK_SPLIT_ROLE_CENTRAL
 * (see Kconfig `depends on`). Peripheral halves never run a second
 * listener — central drives the ramp locally and broadcasts every
 * step to the peripheral via the underglow-state split transport so
 * both halves stay in lock-step. Without that gate, peripheral activity
 * timers would fire independently and the two halves would ramp out
 * of phase.
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
#include <zmk/studio/idle_dimmer.h>

#if IS_ENABLED(CONFIG_ZMK_STUDIO_IDLE_DIMMER)

#define FLOOR_PCT CONFIG_ZMK_STUDIO_IDLE_DIMMER_FLOOR_PCT
#define RAMP_MS CONFIG_ZMK_STUDIO_IDLE_DIMMER_RAMP_MS
/* 16 steps gives a perceptually-smooth taper for the typical 800 ms
 * ramp window. Fewer steps and the eye sees discrete pops; more and
 * we burn idle CPU for sub-perceptual deltas. */
#define STEPS 16
#define STEP_INTERVAL (RAMP_MS / STEPS)

/* Sentinel for "not yet anchored". Anchor lazily on the first IDLE
 * event so settings_load() (called from main() AFTER all SYS_INIT
 * handlers fire) has populated state.color.b with the user's saved
 * baseline. Without this lazy step the eager-init seed would capture
 * CONFIG_ZMK_RGB_UNDERGLOW_BRT_START (e.g. 100) and ramp back to that
 * on every wake — clobbering the user's saved 50% within seconds. */
static uint8_t active_target_pct = 0;
static uint8_t current_pct = 0;
static enum {
    DIRECTION_NONE,
    DIRECTION_DOWN,
    DIRECTION_UP,
} ramp_direction = DIRECTION_NONE;

/* Set true while ramp_step is calling zmk_rgb_underglow_save_state's
 * notify chain on its own write. Lets the notify hook distinguish
 * dimmer-driven brightness writes from user-driven ones — only the
 * latter should re-anchor active_target_pct. Unused today because
 * ramp_step calls set_brightness (which doesn't trigger save_state's
 * notify path), but kept ready for any future caller that wraps the
 * ramp's write through save_state. */
static bool dimmer_owns_write = false;

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
    if (step < 1)
        step = 1;
    if (diff < 0)
        step = -step;

    int next = (int)current_pct + step;
    /* Clamp to target so the last step never overshoots. */
    if ((diff > 0 && next > target) || (diff < 0 && next < target)) {
        next = target;
    }
    current_pct = (uint8_t)next;

    dimmer_owns_write = true;
    zmk_rgb_underglow_set_brightness((uint8_t)current_pct);
    dimmer_owns_write = false;

    if (current_pct != target) {
        k_work_schedule(&ramp_work, K_MSEC(STEP_INTERVAL));
    } else {
        ramp_direction = DIRECTION_NONE;
    }
}

void zmk_rgb_idle_dimmer_notify_user_brightness(uint8_t new_pct) {
    /* Skip our own ramp's writes — those drive `current_pct` already
     * and would otherwise feed back into active_target_pct, dragging
     * the wake target down to the floor. */
    if (dimmer_owns_write) {
        return;
    }
    /* Mid-ramp: don't disturb the in-flight ramp. The next IDLE event
     * re-anchors from the post-ramp baseline (which now includes the
     * user's change because state.color.b is the source of truth). */
    if (ramp_direction != DIRECTION_NONE) {
        return;
    }
    /* Defensive: when CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE layers with
     * the dimmer, AUTO_OFF's wake-up path calls zmk_rgb_underglow_on()
     * which fires save_state() → this notify hook. At that moment
     * state.color.b is still pinned at FLOOR_PCT because the dimmer
     * ramped it down before AUTO_OFF cut EXT_POWER. If listener order
     * happens to put AUTO_OFF before the dimmer's UP-ramp scheduling,
     * ramp_direction is still NONE here — without this guard we'd
     * anchor active_target_pct to FLOOR_PCT and the next ramp UP would
     * target the floor instead of the user's saved brightness, leaving
     * LEDs stuck dim after wake. User-driven brightness changes always
     * go through change_brt/_hsb which clamp at BRT_MIN > FLOOR_PCT
     * typically, so a "real" user write at <= FLOOR is vanishingly
     * unlikely; skip the anchor and let the next IDLE event's
     * re-anchor (which reads state.color.b too but only when
     * ramp_direction == NONE AND live > FLOOR_PCT) pick up the
     * post-wake value once the dimmer's UP ramp completes. */
    if (new_pct <= FLOOR_PCT) {
        return;
    }
    /* User changed brightness while settled at the active target: the
     * new value IS the new active target. Sync both anchors so the
     * next IDLE→ACTIVE ramp targets it. */
    active_target_pct = new_pct;
    current_pct = new_pct;
    LOG_DBG("active brightness re-anchored to %u%%", new_pct);
}

static int on_activity_state_changed(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (!ev)
        return ZMK_EV_EVENT_BUBBLE;

    switch (ev->state) {
    case ZMK_ACTIVITY_ACTIVE:
        if (ramp_direction == DIRECTION_NONE && current_pct == active_target_pct) {
            return ZMK_EV_EVENT_BUBBLE; /* nothing to do */
        }
        /* If we never hit IDLE before the first ACTIVE event (boot is
         * ACTIVE; first transition is ACTIVE→IDLE so this branch only
         * fires after at least one round-trip), active_target_pct is
         * already valid. */
        ramp_direction = DIRECTION_UP;
        k_work_schedule(&ramp_work, K_NO_WAIT);
        break;
    case ZMK_ACTIVITY_IDLE:
    case ZMK_ACTIVITY_SLEEP:
        /* Lazy anchor — first time we hit idle, capture the user's
         * post-settings_load brightness as the wake target. */
        if (active_target_pct == 0) {
            uint8_t live = zmk_rgb_underglow_calc_effective_brightness();
            active_target_pct = (live > FLOOR_PCT) ? live : 100;
            current_pct = live > 0 ? live : active_target_pct;
            LOG_DBG("first-IDLE anchor: target=%u%% live=%u%%", active_target_pct, live);
        } else if (ramp_direction == DIRECTION_NONE) {
            /* Re-anchor on subsequent idles in case user changed brightness
             * via a path that didn't fire the notify hook (defence in depth).
             * Mid-ramp captures would freeze at an intermediate value, hence
             * the DIRECTION_NONE guard. */
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

#endif /* CONFIG_ZMK_STUDIO_IDLE_DIMMER */
