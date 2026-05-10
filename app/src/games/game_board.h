/*
 * AuroraKey games — abstract board interface.
 *
 * Games operate on a logical (x, y) grid of WIDTH × HEIGHT cells. The
 * board interface translates logical cells to physical key positions
 * (0–79 on Glove80) and reports which cells are valid (i.e. have a
 * physical key). Cells that map to -1 are "walls" — a game module
 * either bounces off them, treats them as obstacles, or skips them
 * entirely depending on its mechanics.
 *
 * One board impl per supported keyboard. Glove80 provides the
 * default; future hardware can drop in a parallel impl + a Kconfig
 * select to swap.
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct game_board {
    /* Logical grid dimensions. */
    int width;
    int height;
    /* Inclusive max-x for spawn / food / wrap. Lets games confine
     * mechanics to a sub-rectangle of the full board — used so LH-only
     * mode (CONFIG_AURORAKEY_GAME_FULL_SCREEN=n) doesn't spawn food on
     * RH cells the runtime doesn't paint. Always equals width-1 in
     * full-screen mode. */
    int playable_x_max;
    /* Translate (x, y) → matrix position in [0, ZMK_KEYMAP_LEN) on
     * success, or -1 if the cell has no physical key (wall / gap).
     * Negative coordinates are normalised by the caller; impl can
     * assume 0 ≤ x < width and 0 ≤ y < height. */
    int (*xy_to_pos)(int x, int y);
};

/* The single linked-in board impl. CONFIG_AURORAKEY_GAME_BOARD_*
 * Kconfig select picks which file gets compiled in; the symbol is
 * the same regardless. */
extern const struct game_board game_board;

/* Convenience: returns true when the cell at (x, y) is playable
 * (xy_to_pos returns >= 0). */
static inline bool game_board_cell_is_wall(int x, int y) { return game_board.xy_to_pos(x, y) < 0; }
