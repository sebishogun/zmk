/*
 * AuroraKey games — shared glyph renderer.
 *
 * A tiny 3×5 bitmap glyph blitter shared by every game (originally the
 * Snake "GO" splash). Games define their own 3×5 glyph constants (5
 * bytes, left bit = leftmost pixel) and draw them on the logical board
 * via game_draw_glyph. The runtime reuses it for the cycle name-splash.
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#define GLYPH_W 3
#define GLYPH_H 5

/* Draw a GLYPH_H-row, GLYPH_W-wide bitmap glyph with its top-left at
 * logical board cell (ox, oy). Set bits paint `color`; clear bits are
 * left untouched (paint the background first if you need a box). Bit
 * (GLYPH_W-1) of each row byte is the leftmost pixel. Cells off-board
 * or on walls are silent no-ops (game_paint handles that). */
void game_draw_glyph(const uint8_t rows[GLYPH_H], int ox, int oy, uint32_t color);
