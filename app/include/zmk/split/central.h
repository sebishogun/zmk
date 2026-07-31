/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/bluetooth/addr.h>
#include <zmk/behavior.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)

#include <zmk/ble.h>
#define BLE_PERIPHERAL_COUNT ZMK_SPLIT_BLE_PERIPHERAL_COUNT

#else

#define BLE_PERIPHERAL_COUNT 0

#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_WIRED)
#define WIRED_PERIPHERAL_COUNT 1
#else
#define WIRED_PERIPHERAL_COUNT 0
#endif

#define ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT MAX(BLE_PERIPHERAL_COUNT, WIRED_PERIPHERAL_COUNT)

#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)
#include <zmk/hid_indicators_types.h>
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)

int zmk_split_central_invoke_behavior(uint8_t source, struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event, bool state);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)

int zmk_split_central_update_hid_indicator(zmk_hid_indicators_t indicators);

#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_HID_INDICATORS)

#if IS_ENABLED(CONFIG_EXPERIMENTAL_RGB_LAYER)

int zmk_split_central_update_layers(uint32_t layers);
int zmk_split_central_update_rgb_color(uint32_t layer_id, uint32_t key_pos, uint32_t color);
int zmk_split_central_rgb_save(void);
int zmk_split_central_rgb_discard(void);
int zmk_split_central_rgb_clear_layer(uint32_t layer_id);
/* Peripheral-side counterpart of zmk_rgb_underglow_layer_clear_pending():
 * drops the staged overrides only. Use this to release a layer a transient
 * painter staged into; zmk_split_central_rgb_clear_layer() additionally
 * drops whatever the user committed. */
int zmk_split_central_rgb_clear_layer_pending(uint32_t layer_id);

/* Colour writes are the one split command whose sender is expected to track
 * delivery itself and repaint what did not land. A transport that has to
 * discard an ALREADY-QUEUED colour write — to make room for a command that
 * must land — calls the note function; that write was reported delivered, so
 * nothing else will ever re-send it. Senders keeping a per-key cache should
 * poll the count and drop their cache when it changes, forcing a full repaint.
 * Monotonic and free-running; compare for inequality, not ordering. */
void zmk_split_central_rgb_note_dropped_write(void);
uint32_t zmk_split_central_rgb_dropped_writes(void);

/* Push the full underglow state snapshot to every connected peripheral.
 * Caller passes the values it wants the peripheral to mirror; this
 * function encodes + dispatches via the active transport. Idempotent:
 * sending the same state twice is harmless. Caller is responsible for
 * deciding when to invoke (every state mutation + once after split
 * connection) — this function does not subscribe to anything. */
int zmk_split_central_update_underglow_state(uint16_t h, uint8_t s, uint8_t b, bool on,
                                             uint8_t current_effect, uint8_t animation_speed);

#endif // IS_ENABLED(CONFIG_EXPERIMENTAL_RGB_LAYER)

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)

int zmk_split_central_get_peripheral_battery_level(uint8_t source, uint8_t *level);

#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
