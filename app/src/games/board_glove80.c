/*
 * AuroraKey games — Glove80 board mapping.
 *
 * Maps a logical 14×6 grid onto the Glove80's matrix-transformed key
 * positions. The DT matrix-transform (glove80.dtsi) declares 6 rows
 * of 14 columns with gaps where the wrist sits and an irregular
 * thumb cluster on rows 4–5. We render the games into the regular
 * portion of the grid and treat the gap + thumb cluster cells as
 * walls (return -1) so the game logic sees a coherent rectangle
 * with a few unplayable cells in the middle and bottom.
 *
 * Mapping mirrors the DT matrix-transform map[] verbatim. Updating
 * it requires updating both the DT and this table in lockstep — but
 * the matrix-transform is a Glove80-stable invariant of the board,
 * not something the editor's codegen mutates, so churn is rare.
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>

#include "game_board.h"

#define W 14
#define H 6
/* Playable column range. Full-screen mode = 0..13 (the full Glove80).
 * LH-only mode = 0..5 (LH columns only — cols 6,7 are the wrist gap,
 * 8..13 are the RH side). Compile-time gated off the same Kconfig
 * that controls the split-bt fanout, so the build is guaranteed
 * self-consistent: a UF2 with peripheral fanout enabled has the full
 * 14-col playable area; a UF2 without fanout has the snake confined
 * to the LH strip the user can actually see. */
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_FULL_SCREEN)
#define PLAYABLE_X_MAX (W - 1)
#else
#define PLAYABLE_X_MAX 5
#endif

/* Row-major lookup: -1 = wall, else matrix position 0..79. The middle
 * columns (6, 7) are the wrist gap on EVERY row; row 5 has narrow side
 * gaps where the thumb cluster lives.
 *
 * y=4 used to place RH keys 58/59 in the gap columns (x6/x7), making
 * that one row continuous across the gap. Physically those are the RH
 * bottom row's INNER two keys — the row's six keys sit under columns
 * x8..x13 — so the whole RH bottom row was skewed two columns left.
 * Games saw two phantom 1-key columns in the middle of the board (a
 * Connect 4 cursor crossing them dropped to the bottom row and a piece
 * "landed in place" instantly) while the two outermost RH columns had
 * holes at y4 where their real keys belonged. Aligned to the physical
 * columns; the gap is a wall on every row now. */
static const int8_t xy[H][W] = {
    /* y=0 */ {0, 1, 2, 3, 4, -1, -1, -1, -1, 5, 6, 7, 8, 9},
    /* y=1 */ {10, 11, 12, 13, 14, 15, -1, -1, 16, 17, 18, 19, 20, 21},
    /* y=2 */ {22, 23, 24, 25, 26, 27, -1, -1, 28, 29, 30, 31, 32, 33},
    /* y=3 */ {34, 35, 36, 37, 38, 39, -1, -1, 40, 41, 42, 43, 44, 45},
    /* y=4 */ {46, 47, 48, 49, 50, 51, -1, -1, 58, 59, 60, 61, 62, 63},
    /* y=5 */ {64, 65, 66, 67, 68, -1, -1, -1, -1, 75, 76, 77, 78, 79},
};

static int glove80_xy_to_pos(int x, int y) {
    if (x < 0 || x >= W || y < 0 || y >= H) {
        return -1;
    }
    return (int)xy[y][x];
}

const struct game_board game_board = {
    .width = W,
    .height = H,
    .playable_x_max = PLAYABLE_X_MAX,
    .xy_to_pos = glove80_xy_to_pos,
};
