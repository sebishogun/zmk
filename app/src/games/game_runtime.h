/*
 * AuroraKey games — internal runtime API consumed by per-game modules.
 *
 * Games do not call zmk_rgb_underglow_layer_stage_set directly.
 * Instead they paint through this API:
 *   - game_paint(x, y, rgb)   logical board cell (walls are no-ops)
 *   - game_paint_pos(pos, rgb) any matrix key (thumb-cluster controls)
 *   - game_paint_clear()      clear the playable grid
 *   - game_clear_all()        clear EVERY key incl. the thumb cluster
 * All of these push into the per-key Studio overlay on the active game
 * layer and fan out to the peripheral exactly like a Studio commit.
 *
 * v2 multi-game: each game exports a `const struct game_module`. The
 * runtime holds a Kconfig-built registry of the compiled-in modules,
 * keeps one active, dispatches enter/exit/tick/input to it, and cycles
 * to the next on the reserved CYCLE key. (v1 was a single Kconfig-gated
 * game with no v-table.)
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

/* ─── Paint API ────────────────────────────────────────────────────── */

/* Paint a single logical cell. (x, y) outside the board or on a wall
 * cell is a silent no-op. */
void game_paint(int x, int y, uint32_t color);

/* Clear every cell on the playable board (grid only — does NOT touch
 * the thumb cluster). */
void game_paint_clear(void);

/* Paint a single matrix key position directly (0..ZMK_KEYMAP_LEN-1).
 * The escape hatch for keys not on the logical grid — the thumb-cluster
 * control keys. Out-of-range positions are silent no-ops. */
void game_paint_pos(uint32_t pos, uint32_t color);

/* Clear EVERY key, including the thumb cluster, as individual per-pixel
 * writes (dirty-cached, so cost tracks how many keys were actually
 * lit). The runtime itself no longer calls this — activation and
 * game-cycle blank the canvas with a single fill command instead — but
 * it remains for games that want a mid-session full clear. */
void game_clear_all(void);

/* Set EVERY key — grid and thumb cluster — to one colour on both halves
 * at fill-command cost: one local loop plus a single 9-byte split
 * packet, instead of up to 80 per-pixel writes. Use for full-board
 * moments (death/win wash, board-wide flash, respawn blank); per-pixel
 * painting stays the right tool for everything else. The runtime uses
 * the same primitive to blank the canvas on entry and game-cycle. */
void game_canvas_fill(uint32_t color);

/* ─── Thumb-cluster control keys (Glove80 matrix positions) ────────── */
/*   LH top   [ 52  53  54 ]      RH top   [ 55  56  57 ]
 *   LH bottom[ 69  70  71 ]      RH bottom[ 72  73  74 ]
 * 53 = EXIT (runtime: leave the game layer). 72 = CYCLE (runtime: next
 * game). Everything else is forwarded to the active game's input(). */
#define GKEY_LH_TL 52
#define GKEY_EXIT  53
#define GKEY_LH_TR 54
#define GKEY_RH_TL 55
#define GKEY_RH_TM 56
#define GKEY_RH_TR 57
#define GKEY_LH_BL 69
#define GKEY_LH_BM 70
#define GKEY_LH_BR 71
#define GKEY_CYCLE 72
#define GKEY_RH_BM 73
#define GKEY_RH_BR 74

/* Shared thumb-control legend palette so every game's controls read the
 * same (matches the board feel rather than each game inventing colors). */
#define GAME_CTL_DIR    GAME_COLOR(0x00CCFF) /* arrows / cursor move        */
#define GAME_CTL_SELECT GAME_COLOR(0x00FF66) /* drop / select / start       */
#define GAME_CTL_ALT    GAME_COLOR(0xFFAA00) /* reset / clear / step / etc. */
#define GAME_CTL_EXIT   GAME_COLOR(0xFF0000) /* exit the game layer         */
#define GAME_CTL_CYCLE  GAME_COLOR(0xFFFFFF) /* cycle to the next game      */

/* ─── Game module (v2 vtable) ──────────────────────────────────────── */

struct game_module {
    const char *name;
    const uint8_t *glyph;  /* GLYPH_H bytes (3-wide), shown on cycle. NULL ok. */
    uint32_t glyph_color;  /* tint for the cycle splash */
    void (*enter)(void);
    void (*exit)(void);
    void (*tick)(void);
    void (*input)(uint32_t position);
    int (*tick_ms)(void); /* may be NULL → CONFIG_AURORAKEY_GAME_TICK_MS */
};

#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)
extern const struct game_module snake_module;
#endif
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_CONWAY)
extern const struct game_module conway_module;
#endif
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_CONNECT4)
extern const struct game_module connect4_module;
#endif
