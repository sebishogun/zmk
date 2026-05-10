/*
 * AuroraKey games — internal runtime API consumed by per-game modules.
 *
 * Games do not call zmk_rgb_underglow_layer_stage_set directly.
 * Instead they call game_paint(x, y, rgb) which:
 *   1. Looks up matrix position via game_board.xy_to_pos.
 *   2. Skips wall cells silently (paint on a -1 cell is a no-op).
 *   3. Pushes the colour into the per-key Studio overlay on the
 *      currently-active game layer (so the renderer composites it
 *      and split-bt fans it out to peripheral exactly like a manual
 *      Studio commit).
 *
 * Game modules implement game_xxx_init/enter/exit/tick/input below.
 * The runtime calls them via Kconfig-gated direct calls (no v-table)
 * — single-game-active-at-a-time is the v1 contract.
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "game_board.h"

/* Pack 0xRRGGBB into the 0xEERRGGBB encoding the studio overlay
 * expects (effect=0 / solid). */
#define GAME_COLOR(rgb) ((uint32_t)(0x00 << 24) | ((uint32_t)(rgb) & 0xFFFFFFu))
#define GAME_COLOR_OFF GAME_COLOR(0x000000)

/* Paint a single logical cell. (x, y) outside the board or on a wall
 * cell is a silent no-op. */
void game_paint(int x, int y, uint32_t color);

/* Clear every cell on the playable board. Cheap shortcut — the
 * runtime calls this each tick before the game re-renders. */
void game_paint_clear(void);

/* Per-game module hooks. Bodies are gated by their respective
 * CONFIG_AURORAKEY_GAME_*; the runtime calls only the ones whose
 * gates are set. */
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)
void snake_init(void);
void snake_enter(void);
void snake_exit(void);
void snake_tick(void);
void snake_input(uint32_t position);
int snake_tick_ms(void); /* may slow down / speed up over time */
#endif
