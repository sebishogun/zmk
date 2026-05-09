/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

struct zmk_led_hsb {
    uint16_t h;
    uint8_t s;
    uint8_t b;
};

int zmk_rgb_underglow_toggle(void);
int zmk_rgb_underglow_get_state(bool *state);
int zmk_rgb_underglow_on(void);
int zmk_rgb_underglow_off(void);
int zmk_rgb_underglow_cycle_effect(int direction);
int zmk_rgb_underglow_calc_effect(int direction);
int zmk_rgb_underglow_select_effect(int effect);
struct zmk_led_hsb zmk_rgb_underglow_calc_hue(int direction);
struct zmk_led_hsb zmk_rgb_underglow_calc_sat(int direction);
struct zmk_led_hsb zmk_rgb_underglow_calc_brt(int direction);
int zmk_rgb_underglow_change_hue(int direction);
int zmk_rgb_underglow_change_sat(int direction);
int zmk_rgb_underglow_change_brt(int direction);
int zmk_rgb_underglow_change_spd(int direction);
int zmk_rgb_underglow_set_hsb(struct zmk_led_hsb color);
int zmk_rgb_underglow_set_brightness(uint8_t brightness);
uint8_t zmk_rgb_underglow_calc_effective_brightness(void);
int zmk_rgb_underglow_status(void);

/* Apply a remote underglow state snapshot received from the central
 * over the split transport. Peripheral-only path; central never calls
 * this on itself. Mirrors h/s/b/on/current_effect/animation_speed; does
 * NOT persist to settings (peripheral has no save path for the
 * underglow state struct, by design — central is the source of truth
 * across reboots). Returns 0 on success, -ENODEV if the led_strip
 * isn't ready yet. */
int zmk_rgb_underglow_apply_remote_state(uint16_t h, uint8_t s, uint8_t b, bool on,
                                         uint8_t current_effect, uint8_t animation_speed);

/* Central-side: re-broadcast the current local underglow state to every
 * connected peripheral. Caller fires this from the split-transport
 * GATT-discovery completion path so a fresh-from-boot peripheral catches
 * up to whatever brightness/hue/effect/etc. central had loaded from
 * settings before BLE connected. No-op on peripheral builds and on
 * non-split builds. */
void zmk_rgb_underglow_resync_split_peripheral(void);
