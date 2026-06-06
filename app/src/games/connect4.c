/*
 * AuroraKey games — Connect 4.
 *
 * Two-player turn-based drop game rendered on the per-key RGB LEDs. The
 * logical board (COLS×ROWS, default 6×5, WIN_LEN=4 — all Kconfig, so the
 * rules are general) is mirrored on BOTH halves so each player faces
 * their own copy; the white column cursor shows only on the player whose
 * turn it is.
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
 * center heuristic, 6-9 depth-bounded negamax + alpha-beta. Connect 4 is
 * solved but a full solver is overkill/RAM-heavy on the nRF52840;
 * depth-limited search is plenty strong on a 6×5 board.
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

/* Full 6×5: the Glove80 LED grid is irregular (wrist gap + thumb cluster
 * are wall cells), so a 6-wide board can't be a clean rectangle on one
 * half NOR mirrored on both. Instead the board SPANS the wrist gap —
 * 3 full-height grid columns on each half (LH x2,3,4 + RH x9,10,11) —
 * giving a real 6×5 board plus a top cursor row (y0), all cells valid. */
#ifndef CONFIG_AURORAKEY_GAME_CONNECT4_COLS
#define CONFIG_AURORAKEY_GAME_CONNECT4_COLS 6
#endif
#ifndef CONFIG_AURORAKEY_GAME_CONNECT4_ROWS
#define CONFIG_AURORAKEY_GAME_CONNECT4_ROWS 5
#endif
#ifndef CONFIG_AURORAKEY_GAME_CONNECT4_WIN_LEN
#define CONFIG_AURORAKEY_GAME_CONNECT4_WIN_LEN 4
#endif
#ifndef CONFIG_AURORAKEY_GAME_CONNECT4_AI_LEVEL
#define CONFIG_AURORAKEY_GAME_CONNECT4_AI_LEVEL 5
#endif
#ifndef CONFIG_AURORAKEY_GAME_CONNECT4_TICK_MS
#define CONFIG_AURORAKEY_GAME_CONNECT4_TICK_MS 200
#endif

#define COLS CONFIG_AURORAKEY_GAME_CONNECT4_COLS
#define ROWS CONFIG_AURORAKEY_GAME_CONNECT4_ROWS
#define WIN_LEN CONFIG_AURORAKEY_GAME_CONNECT4_WIN_LEN
#define AI_LEVEL CONFIG_AURORAKEY_GAME_CONNECT4_AI_LEVEL
/* Storage maxima — board fits one Glove80 half (cols 0..5, rows 1..5). */
#define COLS_MAX 6
#define ROWS_MAX 5

#define RED 1
#define YELLOW 2
#define COLOR_RED GAME_COLOR(0xFF0000)
#define COLOR_YELLOW GAME_COLOR(0xFFCC00)
#define COLOR_CURSOR GAME_COLOR(0xFFFFFF)

/* Board column → logical grid x. The board spans the wrist gap using the
 * full-height grid columns of each half (LH x2,3,4 + RH x9,10,11), which
 * are valid on every row y0..5. Board rows render on y1..ROWS, cursor on
 * y0. Supports up to 6 columns; fewer use the centre-most slots. */
static const int8_t c4_gridx[6] = {2, 3, 4, 9, 10, 11};

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
    int8_t board[COLS_MAX][ROWS_MAX]; /* [col][row], row 0 = bottom */
    int8_t height[COLS_MAX];
    enum c4_phase phase;
    int turn;            /* RED / YELLOW */
    enum side side_red;  /* which half plays Red */
    enum side ai_side;   /* SIDE_NONE in 2-player */
    int cursor_col;
    enum side lobby_joiner;
    int64_t lobby_deadline;
    bool ai_pending;
    int64_t ai_think_deadline;
    int winner; /* 0 = draw, else RED/YELLOW */
    int64_t over_started;
};

static struct c4 C;
static int col_order[COLS_MAX]; /* centre-out, for AI ordering + cursor feel */

/* "4" — cycle name-splash glyph. */
static const uint8_t glyph_4[GLYPH_H] = {
    0b101, /* #.#  */
    0b101, /* #.#  */
    0b111, /* ###  */
    0b001, /* ..#  */
    0b001, /* ..#  */
};

/* ─── Board mechanics (operate on the live board; AI mutates + undoes) ─ */

static const int DIRS[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};

static int count_dir(int c, int r, int dc, int dr, int player) {
    int n = 0, cc = c + dc, rr = r + dr;
    while (cc >= 0 && cc < COLS && rr >= 0 && rr < ROWS && C.board[cc][rr] == player) {
        n++;
        cc += dc;
        rr += dr;
    }
    return n;
}

static bool wins_at(int c, int r, int player) {
    for (int d = 0; d < 4; d++) {
        int total = 1 + count_dir(c, r, DIRS[d][0], DIRS[d][1], player) +
                    count_dir(c, r, -DIRS[d][0], -DIRS[d][1], player);
        if (total >= WIN_LEN) {
            return true;
        }
    }
    return false;
}

static bool board_full(void) {
    for (int c = 0; c < COLS; c++) {
        if (C.height[c] < ROWS) {
            return false;
        }
    }
    return true;
}

static void build_col_order(void) {
    int idx = 0;
    int c = COLS / 2;
    col_order[idx++] = c;
    for (int off = 1; idx < COLS; off++) {
        if (c - off >= 0) {
            col_order[idx++] = c - off;
        }
        if (idx < COLS && c + off < COLS) {
            col_order[idx++] = c + off;
        }
    }
}

/* ─── AI ───────────────────────────────────────────────────────────── */

static int eval_for(int player) {
    int opp = 3 - player;
    int score = 0;
    int centre = COLS / 2;
    for (int r = 0; r < ROWS; r++) {
        if (C.board[centre][r] == player) {
            score += 3;
        }
    }
    for (int c = 0; c < COLS; c++) {
        for (int r = 0; r < ROWS; r++) {
            for (int d = 0; d < 4; d++) {
                int pc = 0, oc = 0;
                bool ok = true;
                for (int k = 0; k < WIN_LEN; k++) {
                    int cc = c + DIRS[d][0] * k;
                    int rr = r + DIRS[d][1] * k;
                    if (cc < 0 || cc >= COLS || rr < 0 || rr >= ROWS) {
                        ok = false;
                        break;
                    }
                    int v = C.board[cc][rr];
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
    for (int i = 0; i < COLS; i++) {
        int col = col_order[i];
        if (C.height[col] >= ROWS) {
            continue;
        }
        moved = true;
        int r = C.height[col];
        C.board[col][r] = (int8_t)player;
        C.height[col]++;
        int sc;
        if (wins_at(col, r, player)) {
            sc = AI_WIN - (50 - depth); /* prefer faster wins */
        } else if (depth <= 1) {
            sc = eval_for(player);
        } else {
            sc = -negamax(depth - 1, -beta, -alpha, 3 - player);
        }
        C.height[col]--;
        C.board[col][r] = 0;
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

/* level → search depth. Capped at 6 to keep the recursion comfortably
 * within the system work-queue stack; depth 6 alpha-beta on 6×5 is
 * already very strong. */
static int ai_depth(int level) { return (level < 6) ? 0 : (level - 3); }

static bool move_wins(int col, int player) {
    int r = C.height[col];
    C.board[col][r] = (int8_t)player;
    C.height[col]++;
    bool w = wins_at(col, r, player);
    C.height[col]--;
    C.board[col][r] = 0;
    return w;
}

/* Does dropping at `col` for `player` hand the opponent an immediate win? */
static bool move_gives_win(int col, int player) {
    int opp = 3 - player;
    int r = C.height[col];
    C.board[col][r] = (int8_t)player;
    C.height[col]++;
    bool gives = false;
    for (int j = 0; j < COLS && !gives; j++) {
        if (C.height[j] < ROWS) {
            gives = move_wins(j, opp);
        }
    }
    C.height[col]--;
    C.board[col][r] = 0;
    return gives;
}

static int ai_choose(int player, int level) {
    int legal[COLS_MAX], nl = 0;
    for (int i = 0; i < COLS; i++) {
        int col = col_order[i];
        if (C.height[col] < ROWS) {
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
        int r = C.height[col];
        C.board[col][r] = (int8_t)player;
        C.height[col]++;
        int sc = wins_at(col, r, player) ? AI_WIN : -negamax(depth - 1, -AI_WIN * 8, AI_WIN * 8, 3 - player);
        C.height[col]--;
        C.board[col][r] = 0;
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

static void start_game(enum side ai_side) {
    memset(C.board, 0, sizeof(C.board));
    memset(C.height, 0, sizeof(C.height));
    C.turn = RED;
    C.ai_side = ai_side;
    C.cursor_col = COLS / 2;
    C.phase = C4_PLAY;
    C.ai_pending = false;
    maybe_schedule_ai();
}

static void place_move(int col) {
    if (col < 0 || col >= COLS || C.height[col] >= ROWS) {
        return; /* illegal / full — ignore */
    }
    int color = C.turn;
    int r = C.height[col];
    C.board[col][r] = (int8_t)color;
    C.height[col]++;
    if (wins_at(col, r, color)) {
        C.winner = color;
        C.phase = C4_OVER;
        C.over_started = k_uptime_get();
        return;
    }
    if (board_full()) {
        C.winner = 0;
        C.phase = C4_OVER;
        C.over_started = k_uptime_get();
        return;
    }
    C.turn = 3 - color;
    maybe_schedule_ai();
}

static void cursor_move(int d) {
    int nc = C.cursor_col + d;
    if (nc < 0) {
        nc = 0;
    }
    if (nc >= COLS) {
        nc = COLS - 1;
    }
    C.cursor_col = nc;
}

static void to_lobby(void) {
    memset(&C, 0, sizeof(C));
    C.phase = C4_LOBBY;
    C.lobby_joiner = SIDE_NONE;
    C.ai_side = SIDE_NONE;
    C.side_red = SIDE_LEFT;
    C.cursor_col = COLS / 2;
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
}

static void paint_col_cell(int c, int y, uint32_t color) {
    game_paint(c4_gridx[c], y, color);
}

static void paint_board(void) {
    for (int c = 0; c < COLS; c++) {
        for (int r = 0; r < ROWS; r++) {
            int v = C.board[c][r];
            uint32_t col = (v == RED) ? COLOR_RED : (v == YELLOW) ? COLOR_YELLOW : GAME_COLOR_OFF;
            paint_col_cell(c, ROWS - r, col); /* r=0 bottom → y=ROWS */
        }
    }
}

static void clear_cursor_row(void) {
    for (int c = 0; c < COLS; c++) {
        game_paint(c4_gridx[c], 0, GAME_COLOR_OFF);
    }
}

static void c4_render(void) {
    c4_paint_legend();

    if (C.phase == C4_OVER) {
        int64_t age = k_uptime_get() - C.over_started;
        bool on = ((age / 300) % 2) == 0;
        uint32_t fc = (C.winner == RED)      ? COLOR_RED
                      : (C.winner == YELLOW) ? COLOR_YELLOW
                                             : GAME_COLOR(0x303030); /* draw = grey */
        if (!on) {
            fc = GAME_COLOR_OFF;
        }
        clear_cursor_row();
        for (int c = 0; c < COLS; c++) {
            for (int r = 0; r < ROWS; r++) {
                paint_col_cell(c, ROWS - r, fc);
            }
        }
        game_paint_pos(KEY_RH_DROP, GAME_CTL_SELECT);
        game_paint_pos(KEY_LH_DROP, GAME_CTL_SELECT);
        return;
    }

    paint_board();
    clear_cursor_row();

    if (C.phase == C4_LOBBY) {
        uint8_t b = (uint8_t)(pulse_amp() * 4);
        uint32_t pulse = GAME_COLOR(((uint32_t)b << 16) | ((uint32_t)b << 8) | b);
        game_paint_pos(KEY_RH_DROP, (C.lobby_joiner == SIDE_RIGHT) ? COLOR_RED : pulse);
        game_paint_pos(KEY_LH_DROP, (C.lobby_joiner == SIDE_LEFT) ? COLOR_RED : pulse);
        return;
    }

    /* PLAY: white column cursor on the top row over the active column. */
    game_paint(c4_gridx[C.cursor_col], 0, COLOR_CURSOR);
    enum side as = active_side();
    uint32_t turn_rgb = turn_color_rgb(C.turn);
    /* Active side's drop key lit in the current colour; idle side dim. */
    game_paint_pos(KEY_RH_DROP, (as == SIDE_RIGHT) ? turn_rgb : GAME_COLOR(0x101010));
    game_paint_pos(KEY_LH_DROP, (as == SIDE_LEFT) ? turn_rgb : GAME_COLOR(0x101010));
}

/* ─── Module hooks ─────────────────────────────────────────────────── */

static void c4_enter(void) {
    build_col_order();
    /* Establish a clean grid canvas: cells outside the board region would
     * otherwise bleed the layer's DT colour (only board cells get
     * repainted each frame). On cycle-in the runtime already cleared, so
     * this is mostly a dirty-cache no-op then. */
    game_paint_clear();
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
