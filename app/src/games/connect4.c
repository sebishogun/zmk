/*
 * AuroraKey games — Connect 4.
 *
 * Two-player turn-based drop game rendered on the per-key RGB LEDs
 * across the ENTIRE playable grid — one shared board spanning both
 * halves, columns as physical key columns. The Glove80 grid is ragged
 * (columns are 1–6 keys tall, the wrist gap and thumb cluster are
 * walls), and the rules embrace that instead of carving out a clean
 * rectangle: pieces fall to the lowest empty VALID cell of a column,
 * and a win is WIN_LEN consecutive same-colour cells in a straight
 * line where every step lands on a real key — walls break lines. So a
 * short column simply can't host a vertical four, and lines never
 * jump the wrist gap.
 *
 * The active player's column cursor is the white key at the TOP of the
 * column; it skips full columns. LH player uses the LH thumb arrows,
 * RH player the RH ones, so 2-player works face-to-face on one board.
 *
 * Controls (thumb cluster):
 *   RH player: 55 = ←, 57 = →, 56 = drop
 *   LH player: 69 = ←, 71 = →, 70 = drop
 *   54 = reset to lobby, 53 = exit, 72 = cycle game.
 *
 * Player count is discovered with a join handshake: first DROP claims
 * Red; if the other half joins within LOBBY_WAIT_MS it's 2-player, else
 * the AI takes the open side. The joiner pressing DROP again (or RESET)
 * starts vs the AI immediately.
 *
 * AI (difficulty 1..9, all moves legal): 1-2 random, 3-5 win/block/
 * centre heuristic, 6-9 depth-bounded negamax + alpha-beta. Depth is
 * capped low: the search runs on the system workqueue and the full
 * board branches ~14 wide, so depth buys latency fast.
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

#if IS_ENABLED(CONFIG_AURORAKEY_GAMES) && IS_ENABLED(CONFIG_AURORAKEY_GAME_CONNECT4)

/* Storage maxima — the full logical grid. The board itself is derived
 * from game_board at enter: every column x in [0, playable_x_max] with
 * at least one non-wall cell is in play, at whatever height it has.
 * CONFIG_AURORAKEY_GAME_CONNECT4_COLS / _ROWS still exist in Kconfig
 * (the editor emits them) but are intentionally unused: the board IS
 * the physical grid now, not a configured rectangle floating on it. */
#define GMAX_W 14
#define GMAX_H 6

#ifndef CONFIG_AURORAKEY_GAME_CONNECT4_WIN_LEN
#define CONFIG_AURORAKEY_GAME_CONNECT4_WIN_LEN 4
#endif
#ifndef CONFIG_AURORAKEY_GAME_CONNECT4_AI_LEVEL
#define CONFIG_AURORAKEY_GAME_CONNECT4_AI_LEVEL 5
#endif
#ifndef CONFIG_AURORAKEY_GAME_CONNECT4_TICK_MS
#define CONFIG_AURORAKEY_GAME_CONNECT4_TICK_MS 200
#endif

#define WIN_LEN CONFIG_AURORAKEY_GAME_CONNECT4_WIN_LEN
#define AI_LEVEL CONFIG_AURORAKEY_GAME_CONNECT4_AI_LEVEL

#define RED 1
#define YELLOW 2
#define COLOR_RED GAME_COLOR(0xFF0000)
#define COLOR_YELLOW GAME_COLOR(0xFFCC00)
#define COLOR_CURSOR GAME_COLOR(0xFFFFFF)
/* Empty slots are NOT off — they are the frame, dim blue like the
 * plastic of the physical game. Rendering empties as black made the
 * board invisible: a fresh game was a dark keyboard with one white
 * cursor key, and nothing showed that the whole surface IS the board.
 * Kept well below the piece colours but above the visibility floor
 * (channel values scale by global brightness, so anything under ~0x30
 * disappears at the default 50%). */
#define COLOR_FRAME GAME_COLOR(0x003060)

/* Control keys. */
#define KEY_RH_LEFT 55
#define KEY_RH_DROP 56
#define KEY_RH_RIGHT 57
#define KEY_LH_LEFT 69
#define KEY_LH_DROP 70
#define KEY_LH_RIGHT 71
#define KEY_RESET 54

#define LOBBY_WAIT_MS 5000
#define AI_THINK_MS 600
#define OVER_MS 4500
#define AI_WIN 1000000

enum c4_phase { C4_LOBBY, C4_PLAY, C4_OVER };
enum side { SIDE_NONE = -1, SIDE_LEFT = 0, SIDE_RIGHT = 1 };

struct c4 {
    int8_t grid[GMAX_W][GMAX_H]; /* [x][y], y0 = top row; 0 / RED / YELLOW */
    int8_t fill[GMAX_W];         /* pieces currently in column x */
    enum c4_phase phase;
    int turn;           /* RED / YELLOW */
    enum side side_red; /* which half plays Red */
    enum side ai_side;  /* SIDE_NONE in 2-player */
    int cursor_col;
    enum side lobby_joiner;
    int64_t lobby_deadline;
    bool ai_pending;
    int64_t ai_think_deadline;
    int winner; /* 0 = draw, else RED/YELLOW */
    int64_t over_started;
    int8_t flash_on; /* OVER flash state; -1 = not yet painted */
};

static struct c4 C;

/* Derived geometry — computed once per enter from game_board, constant
 * for the session. cap = key count of the column, top_y = its topmost
 * valid row (where the cursor renders), n_cols = playable_x_max + 1. */
static int8_t col_cap[GMAX_W];
static int8_t col_top_y[GMAX_W];
static int n_cols;
static int col_order[GMAX_W]; /* centre-out, for AI ordering */

/* "4" — cycle name-splash glyph. */
static const uint8_t glyph_4[GLYPH_H] = {
    0b101, /* #.#  */
    0b101, /* #.#  */
    0b111, /* ###  */
    0b001, /* ..#  */
    0b001, /* ..#  */
};

static void init_geometry(void) {
    n_cols = game_board.playable_x_max + 1;
    if (n_cols > GMAX_W) {
        n_cols = GMAX_W;
    }
    for (int x = 0; x < n_cols; x++) {
        col_cap[x] = 0;
        col_top_y[x] = -1;
        for (int y = 0; y < game_board.height && y < GMAX_H; y++) {
            if (!game_board_cell_is_wall(x, y)) {
                col_cap[x]++;
                if (col_top_y[x] < 0) {
                    col_top_y[x] = (int8_t)y;
                }
            }
        }
    }
    /* Centre-out column ordering: good alpha-beta move ordering, and
     * the same list drives "prefer centre" at heuristic levels. */
    int idx = 0;
    int c = n_cols / 2;
    col_order[idx++] = c;
    for (int off = 1; idx < n_cols; off++) {
        if (c - off >= 0) {
            col_order[idx++] = c - off;
        }
        if (idx < n_cols && c + off < n_cols) {
            col_order[idx++] = c + off;
        }
    }
}

/* ─── Board mechanics (operate on the live board; AI mutates + undoes) ─ */

static inline bool cell_valid(int x, int y) {
    return x >= 0 && x < n_cols && y >= 0 && y < game_board.height &&
           !game_board_cell_is_wall(x, y);
}

static bool col_full(int x) { return C.fill[x] >= col_cap[x]; }

/* Row a piece dropped in column x lands on: the lowest (largest y)
 * valid AND empty cell. -1 when the column is full or has no keys.
 * Ragged columns fall out naturally — on a column with a mid-column
 * wall (x12/x13 skip y4) the piece "falls past" the missing key. */
static int drop_y(int x) {
    if (x < 0 || x >= n_cols) {
        return -1;
    }
    for (int y = game_board.height - 1; y >= 0; y--) {
        if (cell_valid(x, y) && C.grid[x][y] == 0) {
            return y;
        }
    }
    return -1;
}

static const int DIRS[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};

/* Count same-player cells from (x,y) walking (dx,dy). A wall, the board
 * edge, or any other value ends the line — lines never cross the wrist
 * gap or a missing key. */
static int count_dir(int x, int y, int dx, int dy, int player) {
    int n = 0, cx = x + dx, cy = y + dy;
    while (cell_valid(cx, cy) && C.grid[cx][cy] == player) {
        n++;
        cx += dx;
        cy += dy;
    }
    return n;
}

static bool wins_at(int x, int y, int player) {
    for (int d = 0; d < 4; d++) {
        int total = 1 + count_dir(x, y, DIRS[d][0], DIRS[d][1], player) +
                    count_dir(x, y, -DIRS[d][0], -DIRS[d][1], player);
        if (total >= WIN_LEN) {
            return true;
        }
    }
    return false;
}

static bool board_full(void) {
    for (int x = 0; x < n_cols; x++) {
        if (!col_full(x)) {
            return false;
        }
    }
    return true;
}

static void place_at(int x, int y, int player) {
    C.grid[x][y] = (int8_t)player;
    C.fill[x]++;
}

static void unplace_at(int x, int y) {
    C.grid[x][y] = 0;
    C.fill[x]--;
}

/* ─── AI ───────────────────────────────────────────────────────────── */

/* Score every WIN_LEN window whose cells are ALL valid — windows that
 * touch a wall or the edge can never complete, so they score nothing.
 * That automatically devalues short columns and gap-adjacent lines. */
static int eval_for(int player) {
    int opp = 3 - player;
    int score = 0;
    for (int x = 0; x < n_cols; x++) {
        for (int y = 0; y < game_board.height; y++) {
            for (int d = 0; d < 4; d++) {
                int pc = 0, oc = 0;
                bool ok = true;
                for (int k = 0; k < WIN_LEN; k++) {
                    int cx = x + DIRS[d][0] * k;
                    int cy = y + DIRS[d][1] * k;
                    if (!cell_valid(cx, cy)) {
                        ok = false;
                        break;
                    }
                    int v = C.grid[cx][cy];
                    if (v == player) {
                        pc++;
                    } else if (v == opp) {
                        oc++;
                    }
                }
                if (!ok) {
                    continue;
                }
                if (pc > 0 && oc == 0) {
                    score += (pc >= WIN_LEN) ? 1000 : (pc == WIN_LEN - 1) ? 12 : (pc == WIN_LEN - 2) ? 4 : 1;
                } else if (oc > 0 && pc == 0) {
                    score -= (oc >= WIN_LEN) ? 1000 : (oc == WIN_LEN - 1) ? 14 : (oc == WIN_LEN - 2) ? 4 : 1;
                }
            }
        }
    }
    return score;
}

/* Depth-bounded negamax + alpha-beta. Returns score from `player`'s
 * perspective. Wins are detected at move time by the caller. */
static int negamax(int depth, int alpha, int beta, int player) {
    int best = -AI_WIN * 4;
    bool moved = false;
    for (int i = 0; i < n_cols; i++) {
        int col = col_order[i];
        int y = drop_y(col);
        if (y < 0) {
            continue;
        }
        moved = true;
        place_at(col, y, player);
        int sc;
        if (wins_at(col, y, player)) {
            sc = AI_WIN - (50 - depth); /* prefer faster wins */
        } else if (depth <= 1) {
            sc = eval_for(player);
        } else {
            sc = -negamax(depth - 1, -beta, -alpha, 3 - player);
        }
        unplace_at(col, y);
        if (sc > best) {
            best = sc;
        }
        if (best > alpha) {
            alpha = best;
        }
        if (alpha >= beta) {
            break;
        }
    }
    if (!moved) {
        return 0; /* draw */
    }
    return best;
}

/* level → search depth. The full board branches ~14 wide and the search
 * runs on the SYSTEM WORKQUEUE, so depth buys wall-clock stall fast:
 * depth 4 with alpha-beta is already tens of thousands of nodes. Capped
 * accordingly — strong play on this ragged board comes mostly from the
 * win/block pre-checks anyway. */
static int ai_depth(int level) {
    switch (level) {
    case 6:
        return 2;
    case 7:
        return 3;
    case 8:
        return 3;
    case 9:
        return 4;
    default:
        return 0;
    }
}

static bool move_wins(int col, int player) {
    int y = drop_y(col);
    if (y < 0) {
        return false;
    }
    place_at(col, y, player);
    bool w = wins_at(col, y, player);
    unplace_at(col, y);
    return w;
}

/* Does dropping at `col` for `player` hand the opponent an immediate win? */
static bool move_gives_win(int col, int player) {
    int opp = 3 - player;
    int y = drop_y(col);
    if (y < 0) {
        return false;
    }
    place_at(col, y, player);
    bool gives = false;
    for (int j = 0; j < n_cols && !gives; j++) {
        gives = move_wins(j, opp);
    }
    unplace_at(col, y);
    return gives;
}

static int ai_choose(int player, int level) {
    int legal[GMAX_W], nl = 0;
    for (int i = 0; i < n_cols; i++) {
        int col = col_order[i];
        if (drop_y(col) >= 0) {
            legal[nl++] = col;
        }
    }
    if (nl == 0) {
        return -1;
    }
    /* Take an immediate win (level >= 2). */
    if (level >= 2) {
        for (int i = 0; i < nl; i++) {
            if (move_wins(legal[i], player)) {
                return legal[i];
            }
        }
    }
    /* Block the opponent's immediate win (level >= 3). */
    if (level >= 3) {
        int opp = 3 - player;
        for (int i = 0; i < nl; i++) {
            if (move_wins(legal[i], opp)) {
                return legal[i];
            }
        }
    }
    int depth = ai_depth(level);
    if (depth <= 0) {
        if (level <= 3) {
            return legal[(int)(sys_rand32_get() % (uint32_t)nl)]; /* random legal */
        }
        if (level == 4) {
            return legal[0]; /* centre-most (legal is centre-ordered) */
        }
        /* level 5: pick the centre-most move that doesn't hand a win. */
        for (int i = 0; i < nl; i++) {
            if (!move_gives_win(legal[i], player)) {
                return legal[i];
            }
        }
        return legal[0];
    }
    /* levels 6-9: negamax. */
    int best = -AI_WIN * 8, bestcol = legal[0];
    for (int i = 0; i < nl; i++) {
        int col = legal[i];
        int y = drop_y(col);
        place_at(col, y, player);
        int sc = wins_at(col, y, player) ? AI_WIN : -negamax(depth - 1, -AI_WIN * 8, AI_WIN * 8, 3 - player);
        unplace_at(col, y);
        if (sc > best) {
            best = sc;
            bestcol = col;
        }
    }
    return bestcol;
}

/* ─── Side / turn helpers ──────────────────────────────────────────── */

static enum side other_side(enum side s) { return (enum side)(1 - (int)s); }
static enum side active_side(void) {
    return (C.turn == RED) ? C.side_red : other_side(C.side_red);
}
static uint32_t turn_color_rgb(int color) { return (color == RED) ? COLOR_RED : COLOR_YELLOW; }

static void maybe_schedule_ai(void) {
    if (C.ai_side != SIDE_NONE && active_side() == C.ai_side) {
        C.ai_pending = true;
        C.ai_think_deadline = k_uptime_get() + AI_THINK_MS;
    } else {
        C.ai_pending = false;
    }
}

/* Keep the cursor on a droppable column, preferring the nearest one. */
static void cursor_normalize(void) {
    if (drop_y(C.cursor_col) >= 0) {
        return;
    }
    for (int off = 1; off < n_cols; off++) {
        if (C.cursor_col - off >= 0 && drop_y(C.cursor_col - off) >= 0) {
            C.cursor_col -= off;
            return;
        }
        if (C.cursor_col + off < n_cols && drop_y(C.cursor_col + off) >= 0) {
            C.cursor_col += off;
            return;
        }
    }
}

static void start_game(enum side ai_side) {
    memset(C.grid, 0, sizeof(C.grid));
    memset(C.fill, 0, sizeof(C.fill));
    C.turn = RED;
    C.ai_side = ai_side;
    /* Start the cursor on a full-height column on Red's side — the
     * centre-most columns are the 1-key stubs by the thumb cluster,
     * which is a terrible place to first spot a white key. */
    C.cursor_col = (C.side_red == SIDE_RIGHT) ? 9 : 4;
    if (C.cursor_col >= n_cols) {
        C.cursor_col = n_cols / 2;
    }
    cursor_normalize();
    C.phase = C4_PLAY;
    C.ai_pending = false;
    maybe_schedule_ai();
}

static void place_move(int col) {
    int y = drop_y(col);
    if (y < 0) {
        return; /* illegal / full — ignore */
    }
    int color = C.turn;
    place_at(col, y, color);
    if (wins_at(col, y, color)) {
        C.winner = color;
        C.phase = C4_OVER;
        C.over_started = k_uptime_get();
        C.flash_on = -1;
        return;
    }
    if (board_full()) {
        C.winner = 0;
        C.phase = C4_OVER;
        C.over_started = k_uptime_get();
        C.flash_on = -1;
        return;
    }
    C.turn = 3 - color;
    cursor_normalize();
    maybe_schedule_ai();
}

/* Skip full columns while moving — the cursor only ever sits where a
 * piece can actually go. */
static void cursor_move(int d) {
    int nc = C.cursor_col;
    for (int step = 0; step < n_cols; step++) {
        nc += d;
        if (nc < 0 || nc >= n_cols) {
            return; /* edge — stay put */
        }
        if (drop_y(nc) >= 0) {
            C.cursor_col = nc;
            return;
        }
    }
}

static void to_lobby(void) {
    memset(&C, 0, sizeof(C));
    C.phase = C4_LOBBY;
    C.lobby_joiner = SIDE_NONE;
    C.ai_side = SIDE_NONE;
    C.side_red = SIDE_LEFT;
    C.cursor_col = n_cols / 2;
    C.flash_on = -1;
    /* One fill paints the entire canvas in frame blue — the whole
     * keyboard visibly becomes the board the instant Connect 4 appears,
     * on both halves at once. It also erases whatever the OVER flash
     * left. Legend and unused keys repaint over it on the next render. */
    game_canvas_fill(COLOR_FRAME);
}

static void lobby_drop(enum side s) {
    if (C.lobby_joiner == SIDE_NONE) {
        C.lobby_joiner = s;
        C.side_red = s; /* first joiner is Red, moves first */
        C.lobby_deadline = k_uptime_get() + LOBBY_WAIT_MS;
    } else if (s != C.lobby_joiner) {
        start_game(SIDE_NONE); /* second human → 2-player */
    } else {
        start_game(other_side(C.lobby_joiner)); /* same joiner again → vs AI now */
    }
}

/* ─── Render ───────────────────────────────────────────────────────── */

static int pulse_amp(void) {
    int ph = (int)((k_uptime_get() / 25) % 80);
    return ph < 40 ? ph : (80 - ph); /* 0..40..0 */
}

static void c4_paint_legend(void) {
    game_paint_pos(KEY_RH_LEFT, GAME_CTL_DIR);
    game_paint_pos(KEY_RH_RIGHT, GAME_CTL_DIR);
    game_paint_pos(KEY_LH_LEFT, GAME_CTL_DIR);
    game_paint_pos(KEY_LH_RIGHT, GAME_CTL_DIR);
    game_paint_pos(KEY_RESET, GAME_CTL_ALT);
    game_paint_pos(GKEY_EXIT, GAME_CTL_EXIT);
    game_paint_pos(GKEY_CYCLE, GAME_CTL_CYCLE);
    /* Thumb keys Connect 4 doesn't use. The lobby fill washes the whole
     * canvas in frame blue, so keys with no role must be explicitly
     * dark or they read as part of the board. */
    game_paint_pos(GKEY_LH_TL, GAME_COLOR_OFF);
    game_paint_pos(GKEY_RH_BM, GAME_COLOR_OFF);
    game_paint_pos(GKEY_RH_BR, GAME_COLOR_OFF);
}

/* Paint every valid cell: piece colour, the active player's cursor on
 * the top key of its column, or OFF. The whole grid is board now, so
 * this is also what erases stale colour — there is no "outside the
 * board" region left to bleed. */
static void paint_board(void) {
    bool show_cursor = (C.phase == C4_PLAY) && !C.ai_pending;
    for (int x = 0; x < n_cols; x++) {
        for (int y = 0; y < game_board.height; y++) {
            if (!cell_valid(x, y)) {
                continue;
            }
            uint32_t col;
            int v = C.grid[x][y];
            if (v == RED) {
                col = COLOR_RED;
            } else if (v == YELLOW) {
                col = COLOR_YELLOW;
            } else if (show_cursor && x == C.cursor_col && y == col_top_y[x]) {
                col = COLOR_CURSOR;
            } else {
                col = COLOR_FRAME; /* empty slot — visibly part of the board */
            }
            game_paint(x, y, col);
        }
    }
}

static void c4_render(void) {
    c4_paint_legend();

    if (C.phase == C4_OVER) {
        int64_t age = k_uptime_get() - C.over_started;
        int8_t on = (int8_t)(((age / 300) % 2) == 0);
        if (on != C.flash_on) {
            C.flash_on = on;
            /* Draw-grey sits above the visibility floor on purpose:
             * channel values are scaled by global brightness (b/255),
             * so at the default 50% a 0x30 channel renders as ~9/255 —
             * invisible. */
            uint32_t fc = (C.winner == RED)      ? COLOR_RED
                          : (C.winner == YELLOW) ? COLOR_YELLOW
                                                 : GAME_COLOR(0x808080); /* draw = grey */
            /* Whole-canvas flash as ONE fill per flip — 68 per-pixel
             * writes per flip overran the split queue and left flash
             * remnants on the peripheral. Legend rides on top. */
            game_canvas_fill(on ? fc : GAME_COLOR_OFF);
            c4_paint_legend();
        }
        game_paint_pos(KEY_RH_DROP, GAME_CTL_SELECT);
        game_paint_pos(KEY_LH_DROP, GAME_CTL_SELECT);
        return;
    }

    paint_board();

    if (C.phase == C4_LOBBY) {
        uint8_t b = (uint8_t)(pulse_amp() * 4);
        uint32_t pulse = GAME_COLOR(((uint32_t)b << 16) | ((uint32_t)b << 8) | b);
        game_paint_pos(KEY_RH_DROP, (C.lobby_joiner == SIDE_RIGHT) ? COLOR_RED : pulse);
        game_paint_pos(KEY_LH_DROP, (C.lobby_joiner == SIDE_LEFT) ? COLOR_RED : pulse);
        return;
    }

    /* PLAY: active side's drop key lit in the current colour; idle side
     * dim so both players always know whose turn it is. */
    enum side as = active_side();
    uint32_t turn_rgb = turn_color_rgb(C.turn);
    game_paint_pos(KEY_RH_DROP, (as == SIDE_RIGHT) ? turn_rgb : GAME_COLOR(0x101010));
    game_paint_pos(KEY_LH_DROP, (as == SIDE_LEFT) ? turn_rgb : GAME_COLOR(0x101010));
}

/* ─── Module hooks ─────────────────────────────────────────────────── */

static void c4_enter(void) {
    init_geometry();
    /* Canvas is already blank — the runtime fills it before enter. */
    to_lobby();
    c4_render();
}

static void c4_exit(void) { /* runtime clears the overlay */ }

static void c4_tick(void) {
    int64_t now = k_uptime_get();
    if (C.phase == C4_LOBBY) {
        if (C.lobby_joiner != SIDE_NONE && now >= C.lobby_deadline) {
            start_game(other_side(C.lobby_joiner)); /* timeout → vs AI */
        }
    } else if (C.phase == C4_PLAY) {
        if (C.ai_pending && now >= C.ai_think_deadline) {
            C.ai_pending = false;
            int col = ai_choose(C.turn, AI_LEVEL);
            if (col >= 0) {
                place_move(col);
            }
        }
    } else if (C.phase == C4_OVER) {
        if (now - C.over_started >= OVER_MS) {
            to_lobby();
        }
    }
    c4_render();
}

static void c4_input(uint32_t position) {
    if (C.phase == C4_LOBBY) {
        if (position == KEY_RH_DROP) {
            lobby_drop(SIDE_RIGHT);
        } else if (position == KEY_LH_DROP) {
            lobby_drop(SIDE_LEFT);
        } else if (position == KEY_RESET) {
            to_lobby();
        }
        c4_render();
        return;
    }
    if (C.phase == C4_OVER) {
        if (position == KEY_RESET || position == KEY_RH_DROP || position == KEY_LH_DROP) {
            to_lobby();
        }
        c4_render();
        return;
    }
    /* PLAY */
    if (position == KEY_RESET) {
        to_lobby();
        c4_render();
        return;
    }
    if (C.ai_pending) {
        c4_render();
        return; /* AI is thinking — ignore input */
    }
    enum side as = active_side();
    if (as == SIDE_RIGHT) {
        if (position == KEY_RH_LEFT) {
            cursor_move(-1);
        } else if (position == KEY_RH_RIGHT) {
            cursor_move(1);
        } else if (position == KEY_RH_DROP) {
            place_move(C.cursor_col);
        }
    } else {
        if (position == KEY_LH_LEFT) {
            cursor_move(-1);
        } else if (position == KEY_LH_RIGHT) {
            cursor_move(1);
        } else if (position == KEY_LH_DROP) {
            place_move(C.cursor_col);
        }
    }
    c4_render();
}

static int c4_tick_ms(void) {
    if (C.phase == C4_PLAY) {
        return C.ai_pending ? 60 : 120; /* pulse cursor / poll AI think timer */
    }
    return 80; /* lobby pulse / over flash */
}

const struct game_module connect4_module = {
    .name = "Connect 4",
    .glyph = glyph_4,
    .glyph_color = COLOR_YELLOW,
    .enter = c4_enter,
    .exit = c4_exit,
    .tick = c4_tick,
    .input = c4_input,
    .tick_ms = c4_tick_ms,
};

#endif /* CONFIG_AURORAKEY_GAMES && CONFIG_AURORAKEY_GAME_CONNECT4 */
