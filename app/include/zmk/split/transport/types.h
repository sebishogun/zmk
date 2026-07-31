/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/hid_indicators_types.h>
#include <zmk/sensors.h>
#include <zephyr/sys/util.h>

enum zmk_split_transport_connections_status {
    ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED = 0,
    ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED,
    ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED,
};

struct zmk_split_transport_status {
    bool available;
    bool enabled;
    enum zmk_split_transport_connections_status connections;
};

typedef struct zmk_split_transport_status (*zmk_split_transport_get_status_t)(void);
typedef int (*zmk_split_transport_set_enabled_t)(bool enabled);

enum zmk_split_transport_peripheral_event_type {
    ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
    ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT,
    ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT,
    ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT,
};

struct zmk_split_transport_peripheral_event {
    enum zmk_split_transport_peripheral_event_type type;

    union {
        struct {
            uint8_t position;
            uint8_t pressed;
        } key_position_event;

        struct {
            struct zmk_sensor_channel_data channel_data;

            uint8_t sensor_index;
        } sensor_event;

        struct {
            uint8_t reg;
            uint8_t sync;
            uint8_t type;
            uint16_t code;
            int32_t value;
        } input_event;

        struct {
            uint8_t level;
        } battery_event;
    } data;
} __packed;

enum zmk_split_transport_central_command_type {
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_POLL_EVENTS,
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_INVOKE_BEHAVIOR,
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_PHYSICAL_LAYOUT,
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_HID_INDICATORS,
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_RGB_LAYERS,
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_RGB_COLOR,
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_RGB_SAVE,
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_RGB_DISCARD,
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_RGB_CLEAR,
    /* AuroraKey: full underglow state mirror (h/s/b/on/effect/speed)
     * pushed central→peripheral on every change AND on connect so RH
     * boots with whatever brightness/hue the user saved on LH instead
     * of CONFIG_ZMK_RGB_UNDERGLOW_BRT_START. Fixes the long-standing
     * "RH starts at 100% even though LH starts at 50%" desync. */
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_UNDERGLOW_STATE,
    /* AuroraKey: clear only the STAGED per-key overrides for a layer,
     * leaving committed ones alone. SET_RGB_CLEAR is the Studio "reset this
     * layer" semantic and drops both; a transient painter like the games
     * runtime stages without ever saving, so using SET_RGB_CLEAR on exit
     * threw away the user's saved colours for the game layer too.
     *
     * Appended rather than slotted in next to SET_RGB_CLEAR: the wired
     * transport serialises this enum by value, so inserting mid-list would
     * renumber every command after it. */
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_RGB_CLEAR_PENDING,
    /* AuroraKey: stage every key of a layer to one colour in a single
     * command. A full-canvas establishment done per pixel is ~80 split
     * writes — the peripheral trails the central by the whole sweep (and
     * under load, part of the sweep can be refused outright), which is
     * why the right half blanked visibly later than the left on every
     * game entry and game-to-game cycle. One fill is one queue slot and
     * one radio packet: both halves flip together. */
    ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_RGB_FILL,
} __packed;

struct zmk_split_transport_central_command {
    enum zmk_split_transport_central_command_type type;

    union {
        struct {
            char behavior_dev[16];
            uint32_t param1, param2;
            uint32_t position;
            uint8_t event_source;
            uint8_t state;
        } invoke_behavior;

        struct {
            uint8_t layout_idx;
        } set_physical_layout;

        struct {
            zmk_hid_indicators_t indicators;
        } set_hid_indicators;

        struct {
            uint32_t layers;
        } set_rgb_layers;

        struct {
            uint32_t layer_id;
            uint32_t key_pos;
            uint32_t color;
        } set_rgb_color;

        struct {
            uint32_t layer_id;
        } set_rgb_clear;

        struct {
            uint32_t layer_id;
            uint32_t color;
        } set_rgb_fill;

        /* Full underglow state snapshot. Sized to the smallest serialisable
         * form of struct rgb_underglow_state — animation_step is excluded
         * (peripheral animates from its own clock so reusing the central's
         * step would just snap), as is status_active (driven entirely by
         * peripheral-side battery / BLE / HID events). */
        struct {
            uint16_t h;
            uint8_t s;
            uint8_t b;
            uint8_t on;
            uint8_t current_effect;
            uint8_t animation_speed;
        } set_underglow_state;
    } data;
} __packed;