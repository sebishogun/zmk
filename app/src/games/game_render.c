/*
 * AuroraKey games — shared glyph renderer (impl).
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>

#include "game_render.h"
#include "game_runtime.h"

#if IS_ENABLED(CONFIG_AURORAKEY_GAMES)

void game_draw_glyph(const uint8_t rows[GLYPH_H], int ox, int oy, uint32_t color) {
    for (int gy = 0; gy < GLYPH_H; gy++) {
        for (int gx = 0; gx < GLYPH_W; gx++) {
            /* Left bit = leftmost pixel, matching the literal layout in
             * the glyph tables (so the bytes read like the picture). */
            if (rows[gy] & (1u << (GLYPH_W - 1 - gx))) {
                game_paint(ox + gx, oy + gy, color);
            }
        }
    }
}

#endif /* CONFIG_AURORAKEY_GAMES */
