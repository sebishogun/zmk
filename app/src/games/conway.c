/*
 * AuroraKey games — Conway's Game of Life.
 *
 * Cellular automaton on the whole Glove80 board (the logical 14×6 grid
 * minus wall cells). Renders only on the per-key RGB LEDs.
 *
 *   EDIT  — white cursor; RH D-pad moves it, LH toggle (69) flips a cell.
 *   RUN   — auto-steps B3/S23 every CONWAY_TICK_MS; no cursor.
 *   PAUSE — frozen.
 *
 * Controls (thumb cluster):
 *   RH 55/56/57/73 = ← ↑ → ↓ cursor
 *   LH 52 = start/pause, 54 = clear, 69 = toggle cell, 70 = step one
 *   generation, 71 = random soup. 53 = exit, 72 = cycle game.
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(aurorakey_games, CONFIG_ZMK_LOG_LEVEL);

#include "game_board.h"
#include "game_render.h"
#include "game_runtime.h"

#if IS_ENABLED(CONFIG_AURORAKEY_GAMES) && IS_ENABLED(CONFIG_AURORAKEY_GAME_CONWAY)

/* Board max — matches the Glove80 logical grid (board_glove80.c W×H). */
#define BMAX_W 14
#define BMAX_H 6

#ifndef CONFIG_AURORAKEY_GAME_CONWAY_TICK_MS
#define CONFIG_AURORAKEY_GAME_CONWAY_TICK_MS 350
#endif
#ifndef CONFIG_AURORAKEY_GAME_CONWAY_INIT_DENSITY
#define CONFIG_AURORAKEY_GAME_CONWAY_INIT_DENSITY 30
#endif
#define CONWAY_WRAP IS_ENABLED(CONFIG_AURORAKEY_GAME_CONWAY_WRAP)

/* Control keys. */
#define KEY_RH_LEFT 55
#define KEY_RH_UP 56
#define KEY_RH_RIGHT 57
#define KEY_RH_DOWN 73
#define KEY_LH_STARTPAUSE 52
#define KEY_LH_CLEAR 54
#define KEY_LH_TOGGLE 69
#define KEY_LH_STEP 70
#define KEY_LH_RANDOM 71

#define COLOR_LIVE GAME_COLOR(0x00FF44)
#define COLOR_CURSOR GAME_COLOR(0xFFFFFF)
#define CURSOR_BLINK_MS 400
#define STABLE_GENS_TO_PAUSE 3 /* auto-pause when nothing changes / extinction */

enum conway_phase { CW_EDIT, CW_RUN, CW_PAUSE };

struct conway {
    uint8_t cells[BMAX_W][BMAX_H];
    uint8_t next[BMAX_W][BMAX_H];
    int cx, cy; /* edit cursor */
    enum conway_phase phase;
    int64_t cursor_ms;
    bool cursor_on;
    int stable_gens;
};

static struct conway C;

/* "L" — cycle name-splash glyph. */
static const uint8_t glyph_L[GLYPH_H] = {
    0b100, /* #..  */
    0b100, /* #..  */
    0b100, /* #..  */
    0b100, /* #..  */
    0b111, /* ###  */
};

/* ─── Rules ────────────────────────────────────────────────────────── */

static bool in_bounds(int x, int y) {
    return x >= 0 && x < game_board.width && y >= 0 && y < game_board.height;
}

static bool alive_at(int x, int y) {
    if (CONWAY_WRAP) {
        x = ((x % game_board.width) + game_board.width) % game_board.width;
        y = ((y % game_board.height) + game_board.height) % game_board.height;
    } else if (!in_bounds(x, y)) {
        return false;
    }
    if (game_board_cell_is_wall(x, y)) {
        return false;
    }
    return C.cells[x][y] != 0;
}

static int neighbours(int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            if (alive_at(x + dx, y + dy)) {
                n++;
            }
        }
    }
    return n;
}

static void step_generation(void) {
    bool changed = false;
    for (int x = 0; x < game_board.width; x++) {
        for (int y = 0; y < game_board.height; y++) {
            if (game_board_cell_is_wall(x, y)) {
                C.next[x][y] = 0;
                continue;
            }
            int n = neighbours(x, y);
            bool a = C.cells[x][y] != 0;
            bool na = a ? (n == 2 || n == 3) : (n == 3); /* B3/S23 */
            C.next[x][y] = na ? 1 : 0;
            if (na != a) {
                changed = true;
            }
        }
    }
    memcpy(C.cells, C.next, sizeof(C.cells));
    if (changed) {
        C.stable_gens = 0;
    } else {
        C.stable_gens++;
    }
}

static void clear_cells(void) {
    memset(C.cells, 0, sizeof(C.cells));
    C.stable_gens = 0;
}

static void random_soup(void) {
    int density = CONFIG_AURORAKEY_GAME_CONWAY_INIT_DENSITY;
    for (int x = 0; x < game_board.width; x++) {
        for (int y = 0; y < game_board.height; y++) {
            if (game_board_cell_is_wall(x, y)) {
                C.cells[x][y] = 0;
                continue;
            }
            C.cells[x][y] = ((int)(sys_rand32_get() % 100) < density) ? 1 : 0;
        }
    }
    C.stable_gens = 0;
}

/* Place the cursor on the nearest non-wall cell at/after (cx,cy). */
static void cursor_to_nonwall(void) {
    int guard = game_board.width * game_board.height;
    while (game_board_cell_is_wall(C.cx, C.cy) && guard-- > 0) {
        C.cx++;
        if (C.cx >= game_board.width) {
            C.cx = 0;
            C.cy++;
            if (C.cy >= game_board.height) {
                C.cy = 0;
            }
        }
    }
}

static void move_cursor(int dx, int dy) {
    if (C.phase != CW_EDIT) {
        C.phase = CW_EDIT; /* touching the D-pad drops into edit mode */
    }
    int nx = C.cx, ny = C.cy;
    int guard = game_board.width + game_board.height;
    do {
        nx += dx;
        ny += dy;
        if (nx < 0 || nx >= game_board.width || ny < 0 || ny >= game_board.height) {
            return; /* edge — stay put */
        }
    } while (game_board_cell_is_wall(nx, ny) && guard-- > 0);
    if (!game_board_cell_is_wall(nx, ny)) {
        C.cx = nx;
        C.cy = ny;
    }
    C.cursor_on = true;
}

/* ─── Render ───────────────────────────────────────────────────────── */

/* Conway carries twice the controls of the other two games and, unlike
 * them, has no obvious goal to infer them from — a first-time player
 * genuinely cannot tell RUN from PAUSE from EDIT, because a settled
 * board and a paused one look identical. So the run/pause key doubles as
 * the phase indicator:
 *
 *   green  RUN   — generations are advancing
 *   amber  PAUSE — frozen, either by you or auto-paused on a still life
 *   cyan   EDIT  — the cursor is live; move it and toggle cells
 *
 * Cyan matches the D-pad colour on purpose: in EDIT the arrows move a
 * cursor rather than doing nothing, so the key that put you there is
 * lit like the keys that became useful. Auto-pause on a stable board
 * (conway.c step_generation) now also announces itself instead of the
 * board just quietly stopping.
 */
static void conway_paint_legend(void) {
    uint32_t phase_color = GAME_CTL_SELECT; /* CW_RUN */
    if (C.phase == CW_PAUSE) {
        phase_color = GAME_CTL_ALT;
    } else if (C.phase == CW_EDIT) {
        phase_color = GAME_CTL_DIR;
    }

    game_paint_pos(KEY_RH_LEFT, GAME_CTL_DIR);
    game_paint_pos(KEY_RH_UP, GAME_CTL_DIR);
    game_paint_pos(KEY_RH_RIGHT, GAME_CTL_DIR);
    game_paint_pos(KEY_RH_DOWN, GAME_CTL_DIR);
    game_paint_pos(KEY_LH_STARTPAUSE, phase_color);
    game_paint_pos(KEY_LH_TOGGLE, GAME_CTL_SELECT);
    game_paint_pos(KEY_LH_CLEAR, GAME_CTL_ALT);
    game_paint_pos(KEY_LH_STEP, GAME_CTL_ALT);
    game_paint_pos(KEY_LH_RANDOM, GAME_CTL_ALT);
    game_paint_pos(GKEY_EXIT, GAME_CTL_EXIT);
    game_paint_pos(GKEY_CYCLE, GAME_CTL_CYCLE);
}

static void conway_render(void) {
    conway_paint_legend();
    for (int x = 0; x < game_board.width; x++) {
        for (int y = 0; y < game_board.height; y++) {
            if (game_board_cell_is_wall(x, y)) {
                continue;
            }
            uint32_t col = C.cells[x][y] ? COLOR_LIVE : GAME_COLOR_OFF;
            if (C.phase == CW_EDIT && C.cursor_on && x == C.cx && y == C.cy) {
                col = COLOR_CURSOR;
            }
            game_paint(x, y, col);
        }
    }
}

/* ─── Module hooks ─────────────────────────────────────────────────── */

static void conway_enter(void) {
    clear_cells();
    random_soup(); /* land on something alive so entry isn't a blank board */
    C.cx = game_board.width / 2;
    C.cy = game_board.height / 2;
    cursor_to_nonwall();
    C.phase = CW_RUN;
    C.cursor_on = true;
    C.cursor_ms = k_uptime_get();
    C.stable_gens = 0;
    conway_render();
}

static void conway_exit(void) { /* runtime clears the overlay */ }

static void conway_tick(void) {
    int64_t now = k_uptime_get();
    if (C.phase == CW_RUN) {
        step_generation();
        if (C.stable_gens >= STABLE_GENS_TO_PAUSE) {
            C.phase = CW_PAUSE; /* still life / oscillator settled / extinct */
        }
    } else if (C.phase == CW_EDIT) {
        if (now - C.cursor_ms >= CURSOR_BLINK_MS) {
            C.cursor_on = !C.cursor_on;
            C.cursor_ms = now;
        }
    }
    conway_render();
}

static void conway_input(uint32_t position) {
    switch (position) {
    case KEY_RH_LEFT:
        move_cursor(-1, 0);
        break;
    case KEY_RH_RIGHT:
        move_cursor(1, 0);
        break;
    case KEY_RH_UP:
        move_cursor(0, -1);
        break;
    case KEY_RH_DOWN:
        move_cursor(0, 1);
        break;
    case KEY_LH_TOGGLE:
        if (C.phase != CW_EDIT) {
            C.phase = CW_EDIT;
        }
        if (!game_board_cell_is_wall(C.cx, C.cy)) {
            C.cells[C.cx][C.cy] ^= 1;
        }
        C.stable_gens = 0;
        C.cursor_on = true;
        break;
    case KEY_LH_STARTPAUSE:
        C.phase = (C.phase == CW_RUN) ? CW_PAUSE : CW_RUN;
        break;
    case KEY_LH_CLEAR:
        clear_cells();
        C.phase = CW_EDIT;
        C.cursor_on = true;
        break;
    case KEY_LH_STEP:
        step_generation();
        C.phase = CW_PAUSE; /* single-step leaves it paused */
        break;
    case KEY_LH_RANDOM:
        random_soup();
        C.phase = CW_RUN;
        break;
    default:
        break;
    }
    conway_render();
}

static int conway_tick_ms(void) {
    if (C.phase == CW_RUN) {
        return CONFIG_AURORAKEY_GAME_CONWAY_TICK_MS;
    }
    return 120; /* responsive cursor blink / edit feedback */
}

const struct game_module conway_module = {
    .name = "Life",
    .glyph = glyph_L,
    .glyph_color = COLOR_LIVE,
    .enter = conway_enter,
    .exit = conway_exit,
    .tick = conway_tick,
    .input = conway_input,
    .tick_ms = conway_tick_ms,
};

#endif /* CONFIG_AURORAKEY_GAMES && CONFIG_AURORAKEY_GAME_CONWAY */
