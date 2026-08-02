/*
 * AuroraKey games — Snake.
 *
 * Classic snake on a logical 14×6 grid (whatever game_board reports).
 * Production-feature checklist:
 *
 *   - Direction changes buffered into a small queue so quick rotations
 *     never cancel themselves; consumed at the start of each tick.
 *   - 180° reversal blocked (can't immediately walk back along the body).
 *   - Wall cells (-1 from board.xy_to_pos) are passable: the snake walks
 *     "through" the gap and the wrap math jumps over them, so the
 *     splayed Glove80 wrist gap doesn't end the run.
 *   - Torus wrap on the logical edges (configurable via
 *     CONFIG_AURORAKEY_GAME_SNAKE_WRAP). When wrap is off, walking
 *     into the edge dies.
 *   - Self-collision detection. Tail moves out of the way so growing
 *     the body to length L can't immediately self-collide on cells L−1.
 *   - Win: snake length reaches the playable cell count → flash green
 *     until reset.
 *   - Death: flash red for ~1.5 s, then auto-reset to a fresh game.
 *   - Speedup: optional decrement to tick interval per food eaten,
 *     down to a floor of 60 ms.
 *   - Pause: LH start key toggles. Paused state pulses the body so the
 *     user can see the game is alive (vs the LEDs going dark).
 *
 * State lives in a single static struct; the runtime instantiates one
 * Snake per build. Multi-instance is not a goal.
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

#if IS_ENABLED(CONFIG_AURORAKEY_GAMES) && IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)

#define SNAKE_MAX_LEN 80 /* covers the entire Glove80 grid */
#define DIR_BUFFER 4

#define KEY_LH_START 52
#define KEY_LH_RESET 54
/* RH thumb cluster — D-pad layout matching the physical key positions:
 *   top row    [ 55  56  57 ]   left up right
 *   bottom row [ 72  73  74 ]   . down .
 * 73 (bottom-middle) is the only bottom-row binding so the user can rest
 * their thumb naturally and reach all four directions without crossing
 * fingers. */
#define KEY_RH_LEFT 55
#define KEY_RH_UP 56
#define KEY_RH_RIGHT 57
#define KEY_RH_DOWN 73

/* Flash duration on death / win. Pure cosmetic — gameplay is paused. */
#define SNAKE_DEATH_FLASH_MS 1500
#define SNAKE_WIN_FLASH_MS 4000

/* Speedup: tick *= (100 - ACCEL_PCT) / 100 per food, floored at MIN_MS.
 * Default ACCEL_PCT=0 keeps a steady pace, which is what most users
 * want; users who like progressively-harder Snake tick it up. */
#define ACCEL_PCT CONFIG_AURORAKEY_GAME_SNAKE_ACCEL_PCT
#define MIN_TICK_MS 60

/* No WARMUP phase any more: the canvas is established by the runtime's
 * fill command (one packet, both halves), on entry and on every reset
 * via game_canvas_fill(). The old paced OFF-painting pass predates that
 * primitive and had become a ~1 s dead wait. */
enum snake_phase {
    PHASE_INTRO,
    PHASE_PLAYING,
    PHASE_PAUSED,
    PHASE_DEAD,
    PHASE_WON,
};

#define INTRO_MS 1500
/* DEAD / WON: how long the full-board wash (a single canvas fill) stays
 * solid before the render falls back to body-only flash for the rest of
 * SNAKE_DEATH_FLASH_MS / SNAKE_WIN_FLASH_MS. */
#define FULLBOARD_FLASH_MS 1000

struct cell {
    int8_t x;
    int8_t y;
};

struct snake {
    struct cell body[SNAKE_MAX_LEN]; /* head at index 0 */
    int len;
    int dx, dy; /* current movement direction */
    /* Direction buffer absorbs quick consecutive direction presses
     * during a single tick window so they all register; consumed
     * head-first at the start of the next tick. */
    struct cell dir_buffer[DIR_BUFFER];
    int dir_buffer_len;
    struct cell food;
    int tick_ms;
    enum snake_phase phase;
    int64_t phase_started_ms;
    /* Body pulse on pause: a 0..1..0 ramp over PAUSE_PULSE_MS. */
    int playable_cells;
    /* Diff-render state: cells we lit last frame so we can erase any
     * that aren't lit this frame. Drops per-tick fanout from 80 cells
     * (full-board paint_clear) to ~3 cells (head moves, tail vacates,
     * food maybe), which is the difference between BLE saturation and
     * comfortable both-halves rendering. */
    struct cell prev_body[SNAKE_MAX_LEN];
    int prev_len;
    struct cell prev_food;
    bool prev_food_valid;
};

static struct snake S;

/* ─── Intro splash ─────────────────────────────────────────────────── */

/* "GO" rendered as a 3-col × 5-row bitmap per letter. G lives on the
 * LH side (logical cols 1..3, rows 0..4); O lives on the RH side
 * (logical cols 9..11, rows 0..4). Spans both halves visually so
 * the user immediately sees both LEDs strips coming alive. Cells in
 * the middle wrist gap aren't used — the splayed layout means
 * "GO" doesn't read as continuous text either way; LH = G, RH = O
 * is the cleanest mapping. */
static const uint8_t glyph_G[GLYPH_H] = {
    0b111, /* ###  */
    0b100, /* #..  */
    0b101, /* #.#  */
    0b101, /* #.#  */
    0b011, /* .##  */
};
static const uint8_t glyph_O[GLYPH_H] = {
    0b010, /* .#.  */
    0b101, /* #.#  */
    0b101, /* #.#  */
    0b101, /* #.#  */
    0b010, /* .#.  */
};

/* "S" — the cycle name-splash glyph (runtime draws games[active]->glyph). */
static const uint8_t glyph_S[GLYPH_H] = {
    0b111, /* ###  */
    0b100, /* #..  */
    0b111, /* ###  */
    0b001, /* ..#  */
    0b111, /* ###  */
};

/* Glyph blitting now lives in game_render.c (game_draw_glyph) — shared
 * with the other games and the runtime's cycle splash. */

/* ─── Helpers ──────────────────────────────────────────────────────── */

static int wrap(int v, int max) {
    /* Positive-modulo so callers can pass v < 0. */
    return ((v % max) + max) % max;
}

static int playable_cell_count(void) {
    int n = 0;
    for (int y = 0; y < game_board.height; y++) {
        for (int x = 0; x <= game_board.playable_x_max; x++) {
            if (!game_board_cell_is_wall(x, y)) {
                n++;
            }
        }
    }
    return n;
}

static bool cell_in_body(int x, int y, int up_to) {
    for (int i = 0; i < up_to; i++) {
        if (S.body[i].x == x && S.body[i].y == y) {
            return true;
        }
    }
    return false;
}

static bool cell_at_food(int x, int y) { return S.food.x == x && S.food.y == y; }

static void place_food(void) {
    /* Bounded retry: try N random cells, fall back to a deterministic
     * scan if RNG keeps landing on the body (only matters when the
     * snake fills most of the board). Accepts walls in the bounded
     * loop so we don't bias against rare empty cells. */
    uint32_t playable_w = (uint32_t)(game_board.playable_x_max + 1);
    for (int i = 0; i < 64; i++) {
        int x = (int)(sys_rand32_get() % playable_w);
        int y = (int)(sys_rand32_get() % (uint32_t)game_board.height);
        if (game_board_cell_is_wall(x, y)) {
            continue;
        }
        if (cell_in_body(x, y, S.len)) {
            continue;
        }
        S.food.x = (int8_t)x;
        S.food.y = (int8_t)y;
        return;
    }
    /* Fallback: scan in row-major order. */
    for (int y = 0; y < game_board.height; y++) {
        for (int x = 0; x <= game_board.playable_x_max; x++) {
            if (game_board_cell_is_wall(x, y)) {
                continue;
            }
            if (cell_in_body(x, y, S.len)) {
                continue;
            }
            S.food.x = (int8_t)x;
            S.food.y = (int8_t)y;
            return;
        }
    }
    /* Board full = win condition handled by the tick logic. */
}

static int snake_default_tick_ms(void) {
#ifdef CONFIG_AURORAKEY_GAME_SNAKE_TICK_MS
    int v = CONFIG_AURORAKEY_GAME_SNAKE_TICK_MS;
    if (v <= 0)
        v = CONFIG_AURORAKEY_GAME_TICK_MS;
#else
    int v = CONFIG_AURORAKEY_GAME_TICK_MS;
#endif
    /* Tighter playable area → quicker feel. LH-only's 6-col strip
     * needs ~75% of the full-screen tick to play right; same 200 ms
     * on a half-width board makes the snake feel sluggish because
     * reaction distance is shorter. */
    if (game_board.playable_x_max < game_board.width - 1) {
        v = (v * 3) / 4;
    }
    return v;
}

static void snake_reset(void) {
    memset(&S, 0, sizeof(S));
    S.len = 3;
    /* Spawn near the centre of the playable area on a non-wall row.
     * Anchored on playable_x_max so LH-only mode (6-col strip) spawns
     * the snake on the LH side, not floating on the wrist gap. */
    int playable_w = game_board.playable_x_max + 1;
    int cx = playable_w / 2;
    int cy = game_board.height / 2;
    /* Walk left until we find 3 consecutive non-wall cells for the
     * starting body. If the board is degenerate (no such run) we fall
     * back to the first non-wall cell. */
    int sx = cx, sy = cy;
    for (int probe = 0; probe < playable_w; probe++) {
        int x = wrap(cx - probe, playable_w);
        bool ok = true;
        for (int k = 0; k < 3; k++) {
            int xk = wrap(x + k, playable_w);
            if (game_board_cell_is_wall(xk, cy)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            sx = x;
            sy = cy;
            break;
        }
    }
    S.body[0].x = (int8_t)wrap(sx + 2, playable_w);
    S.body[0].y = (int8_t)sy;
    S.body[1].x = (int8_t)wrap(sx + 1, playable_w);
    S.body[1].y = (int8_t)sy;
    S.body[2].x = (int8_t)sx;
    S.body[2].y = (int8_t)sy;
    S.dx = 1;
    S.dy = 0;
    S.dir_buffer_len = 0;
    S.tick_ms = snake_default_tick_ms();
    /* Blank the whole canvas in one fill — on a fresh entry this is a
     * cheap no-op repeat of the runtime's own fill; after a death/win
     * it is what erases the red/green wash instantly on both halves.
     * Then straight into the GO splash. */
    game_canvas_fill(GAME_COLOR_OFF);
    S.phase = PHASE_INTRO;
    S.phase_started_ms = k_uptime_get();
    S.playable_cells = playable_cell_count();
    place_food();
}

/* ─── Direction handling ───────────────────────────────────────────── */

static void buffer_direction(int dx, int dy) {
    /* Compute against the most-recent buffered direction (or current
     * direction if buffer empty) to block 180° reversal on a quick
     * double-tap. */
    int last_dx, last_dy;
    if (S.dir_buffer_len > 0) {
        last_dx = S.dir_buffer[S.dir_buffer_len - 1].x;
        last_dy = S.dir_buffer[S.dir_buffer_len - 1].y;
    } else {
        last_dx = S.dx;
        last_dy = S.dy;
    }
    if (dx == -last_dx && dy == -last_dy) {
        return; /* reversal blocked */
    }
    if (S.dir_buffer_len >= DIR_BUFFER) {
        /* Buffer full; drop oldest queued direction (FIFO eviction). */
        for (int i = 1; i < DIR_BUFFER; i++) {
            S.dir_buffer[i - 1] = S.dir_buffer[i];
        }
        S.dir_buffer_len = DIR_BUFFER - 1;
    }
    S.dir_buffer[S.dir_buffer_len].x = (int8_t)dx;
    S.dir_buffer[S.dir_buffer_len].y = (int8_t)dy;
    S.dir_buffer_len++;
}

static void consume_one_buffered_direction(void) {
    if (S.dir_buffer_len == 0) {
        return;
    }
    int dx = S.dir_buffer[0].x;
    int dy = S.dir_buffer[0].y;
    /* Sanity check against the now-current direction; reversal guard
     * already ran on enqueue but a 3-step rotation could enqueue the
     * reverse via two intermediate quarter-turns — accept those, the
     * earlier turns played out. */
    S.dx = dx;
    S.dy = dy;
    for (int i = 1; i < S.dir_buffer_len; i++) {
        S.dir_buffer[i - 1] = S.dir_buffer[i];
    }
    S.dir_buffer_len--;
}

/* ─── Tick / step ──────────────────────────────────────────────────── */

static void enter_phase(enum snake_phase phase) {
    S.phase = phase;
    S.phase_started_ms = k_uptime_get();
    if (phase == PHASE_DEAD || phase == PHASE_WON) {
        /* Full-board wash in ONE fill — both halves flash together.
         * The paced per-cell version walked past refused writes and
         * left artifacts on the peripheral that survived into the next
         * run. The legend is repainted by the very next render pass. */
        game_canvas_fill(phase == PHASE_DEAD ? GAME_COLOR(0xFF0000) : GAME_COLOR(0x00FF00));
    }
}

static void step_playing(void) {
    consume_one_buffered_direction();
    int nx = S.body[0].x + S.dx;
    int ny = S.body[0].y + S.dy;
    int playable_w = game_board.playable_x_max + 1;
    bool wrap_enabled = IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE_WRAP);
    if (wrap_enabled) {
        /* Wrap on the playable strip width, not the full board width —
         * keeps the snake on cells the user can see. */
        nx = wrap(nx, playable_w);
        ny = wrap(ny, game_board.height);
    } else {
        if (nx < 0 || nx >= playable_w || ny < 0 || ny >= game_board.height) {
            enter_phase(PHASE_DEAD);
            return;
        }
    }
    /* Step over wall cells: continue in the same direction, with a
     * bounded safety so a fully-walled row doesn't hang the tick. */
    int safety = playable_w + game_board.height;
    while (game_board_cell_is_wall(nx, ny) && safety-- > 0) {
        nx += S.dx;
        ny += S.dy;
        if (wrap_enabled) {
            nx = wrap(nx, playable_w);
            ny = wrap(ny, game_board.height);
        } else if (nx < 0 || nx >= playable_w || ny < 0 || ny >= game_board.height) {
            enter_phase(PHASE_DEAD);
            return;
        }
    }
    bool ate = cell_at_food(nx, ny);
    /* Self-collision: ignore the tail when not growing — it'll move
     * out of the new head's cell. */
    int collide_max = ate ? S.len : (S.len - 1);
    if (collide_max < 0) {
        collide_max = 0;
    }
    if (cell_in_body(nx, ny, collide_max)) {
        enter_phase(PHASE_DEAD);
        return;
    }
    int new_len = ate ? (S.len + 1) : S.len;
    if (new_len > SNAKE_MAX_LEN) {
        new_len = SNAKE_MAX_LEN;
    }
    /* Shift the body down so cells[i] = old cells[i-1]. Walk back so
     * the writes don't trample the source. */
    for (int i = new_len - 1; i > 0; i--) {
        S.body[i] = S.body[i - 1];
    }
    S.body[0].x = (int8_t)nx;
    S.body[0].y = (int8_t)ny;
    S.len = new_len;
    if (ate) {
        if (S.len >= S.playable_cells) {
            enter_phase(PHASE_WON);
            return;
        }
        place_food();
        if (ACCEL_PCT > 0 && ACCEL_PCT < 100) {
            int next = (S.tick_ms * (100 - ACCEL_PCT)) / 100;
            if (next < MIN_TICK_MS) {
                next = MIN_TICK_MS;
            }
            S.tick_ms = next;
        }
    }
}

/* ─── Render ───────────────────────────────────────────────────────── */

/* Render the "GO" intro splash. G on the LH side (logical cols 1..3,
 * rows 0..4); O on the RH side (cols 9..11, rows 0..4). Both sides
 * of the keyboard light up so the user immediately sees both halves
 * are alive. Pulses brightness over the intro window for a "fade-in"
 * feel — at t=0 it's dim, peaks at t=INTRO_MS/2, settles to full
 * just before the game starts. */
static void render_intro(void) {
    /* Don't repaint per tick — the reset's canvas fill established the
     * OFF background, and any per-tick brightness change would push 18
     * letter cells × 20 Hz = 360 BLE writes/sec over the link. Paint
     * each letter once at full brightness; the dirty cache
     * short-circuits every later tick. */
    int64_t age = k_uptime_get() - S.phase_started_ms;
    if (age < 0) {
        age = 0;
    }
    /* White-ish with a slight cyan tint so the letters read clearly
     * against the off background without looking sickly green. */
    uint32_t color = GAME_COLOR(0xDCF0FF);
    if (game_board.playable_x_max >= 11) {
        /* Full-screen: G on LH cols 1..3, O on RH cols 9..11. Both
         * letters appear together at t=0 — both halves are visible so
         * the user reads "GO" simultaneously. */
        game_draw_glyph(glyph_G, 1, 0, color);
        game_draw_glyph(glyph_O, 9, 0, color);
    } else {
        /* LH-only: G + O don't fit side-by-side at 3 cols each on a
         * 6-col strip, but sequencing them reads better as a "GO"
         * cue. G appears at t=0; O joins at the halfway mark on cols
         * 3..5. By the end of the intro both letters are lit. */
        game_draw_glyph(glyph_G, 0, 0, color);
        if (age >= INTRO_MS / 2) {
            game_draw_glyph(glyph_O, 3, 0, color);
        }
    }
}

/* Steady-state diff-only render. Erases cells the snake vacated since
 * last frame, paints the new body + food. ~3 cell-pushes per tick in
 * common case (head moves, tail vacates, food unchanged) versus 80 if
 * we re-painted the whole board. Critical for both-halves mode where
 * the BLE link can only sustain ~100 writes/sec. */
static void render_diff(void) {
    /* 1. Erase cells in last frame's body that aren't in the current
     *    body. game_paint(OFF) on cells already cached as OFF is a
     *    no-op via the dirty cache, so collisions cost nothing. */
    for (int i = 0; i < S.prev_len; i++) {
        struct cell c = S.prev_body[i];
        if (!cell_in_body(c.x, c.y, S.len)) {
            game_paint(c.x, c.y, GAME_COLOR_OFF);
        }
    }
    /* 2. Erase old food cell if it moved. */
    if (S.prev_food_valid && (S.prev_food.x != S.food.x || S.prev_food.y != S.food.y)) {
        game_paint(S.prev_food.x, S.prev_food.y, GAME_COLOR_OFF);
    }
    /* 3. Compute body colours from phase. */
    uint32_t head_color, body_color;
    switch (S.phase) {
    case PHASE_DEAD:
        head_color = GAME_COLOR(0xFF0000);
        body_color = GAME_COLOR(0x800000);
        break;
    case PHASE_WON:
        head_color = GAME_COLOR(0x00FF00);
        body_color = GAME_COLOR(0x00FF00);
        break;
    case PHASE_PAUSED: {
        /* Slow pulse on the body so the user knows the game is on. */
        int64_t pause_age = k_uptime_get() - S.phase_started_ms;
        int phase_t = (int)((pause_age / 25) % 80);
        int amp = phase_t < 40 ? phase_t : (80 - phase_t);
        uint8_t b = (uint8_t)(amp * 4);
        head_color = GAME_COLOR((uint32_t)b << 8);
        body_color = GAME_COLOR(((uint32_t)(b / 2)) << 8);
        break;
    }
    case PHASE_INTRO: /* unreachable */
    case PHASE_PLAYING:
    default:
        head_color = GAME_COLOR(0x00FF00);
        body_color = GAME_COLOR(0x004000);
        break;
    }
    /* 4. Paint food first (head wins if there's any overlap). */
    if (S.phase == PHASE_PLAYING || S.phase == PHASE_PAUSED) {
        game_paint(S.food.x, S.food.y, GAME_COLOR(0xFF0000));
    }
    /* 5. Paint body tail-first so head paints last. */
    for (int i = S.len - 1; i >= 0; i--) {
        uint32_t c = (i == 0) ? head_color : body_color;
        game_paint(S.body[i].x, S.body[i].y, c);
    }
    /* 6. Save state for next frame's diff pass. */
    for (int i = 0; i < S.len; i++) {
        S.prev_body[i] = S.body[i];
    }
    S.prev_len = S.len;
    S.prev_food = S.food;
    S.prev_food_valid = true;
}

/* Paint the thumb-cluster control legend in the shared palette so the
 * game's keys are lit like the rest of the board. Dirty-cached → ~free
 * after the first frame. LH bottom row (69/70/71) is unused by Snake. */
static void snake_paint_legend(void) {
    game_paint_pos(KEY_RH_LEFT, GAME_CTL_DIR);
    game_paint_pos(KEY_RH_UP, GAME_CTL_DIR);
    game_paint_pos(KEY_RH_RIGHT, GAME_CTL_DIR);
    game_paint_pos(KEY_RH_DOWN, GAME_CTL_DIR);
    game_paint_pos(KEY_LH_START, GAME_CTL_SELECT);
    game_paint_pos(KEY_LH_RESET, GAME_CTL_ALT);
    game_paint_pos(GKEY_EXIT, GAME_CTL_EXIT);
    game_paint_pos(GKEY_CYCLE, GAME_CTL_CYCLE);
}

static void render(void) {
    snake_paint_legend();
    if (S.phase == PHASE_INTRO) {
        render_intro();
        return;
    }
    /* DEAD / WON: enter_phase already washed the whole board red/green
     * with a single fill, so while the solid-flash window runs there is
     * nothing to paint (the legend above rides on top of the wash).
     * After it elapses, settle into the body-only flash for the rest of
     * the wait. */
    int64_t age = k_uptime_get() - S.phase_started_ms;
    if ((S.phase == PHASE_DEAD || S.phase == PHASE_WON) && age < FULLBOARD_FLASH_MS) {
        return;
    }
    render_diff();
}

/* ─── Game module hooks ────────────────────────────────────────────── */

static void snake_enter(void) {
    snake_reset();
    render();
}

static void snake_exit(void) { /* Nothing to do; runtime clears the layer's overlay. */ }

static void snake_tick(void) {
    int64_t now = k_uptime_get();
    switch (S.phase) {
    case PHASE_INTRO:
        if (now - S.phase_started_ms >= INTRO_MS) {
            S.phase = PHASE_PLAYING;
            S.phase_started_ms = now;
            /* Wipe the GO letters so render_diff doesn't leave them
             * lit forever. Only the ~18 letter cells actually push
             * (every other cell is cached as OFF from warmup), and
             * the dirty cache short-circuits the rest. The first
             * PLAYING tick then paints the snake on a clean canvas. */
            game_paint_clear();
        }
        break;
    case PHASE_PLAYING:
        step_playing();
        break;
    case PHASE_PAUSED:
        /* idle */
        break;
    case PHASE_DEAD:
        if (now - S.phase_started_ms >= SNAKE_DEATH_FLASH_MS) {
            snake_reset();
        }
        break;
    case PHASE_WON:
        if (now - S.phase_started_ms >= SNAKE_WIN_FLASH_MS) {
            snake_reset();
        }
        break;
    }
    render();
}

static void snake_input(uint32_t position) {
    switch (position) {
    case KEY_RH_UP:
        if (S.phase == PHASE_PLAYING) {
            buffer_direction(0, -1);
        }
        break;
    case KEY_RH_DOWN:
        if (S.phase == PHASE_PLAYING) {
            buffer_direction(0, 1);
        }
        break;
    case KEY_RH_LEFT:
        if (S.phase == PHASE_PLAYING) {
            buffer_direction(-1, 0);
        }
        break;
    case KEY_RH_RIGHT:
        if (S.phase == PHASE_PLAYING) {
            buffer_direction(1, 0);
        }
        break;
    case KEY_LH_START:
        if (S.phase == PHASE_INTRO) {
            /* Skip the splash — start playing immediately. */
            S.phase = PHASE_PLAYING;
            S.phase_started_ms = k_uptime_get();
            game_paint_clear();
        } else if (S.phase == PHASE_PLAYING) {
            enter_phase(PHASE_PAUSED);
        } else if (S.phase == PHASE_PAUSED) {
            enter_phase(PHASE_PLAYING);
        } else {
            /* DEAD / WON: skip the auto-reset wait, restart now. */
            snake_reset();
        }
        break;
    case KEY_LH_RESET:
        snake_reset();
        break;
    default:
        break;
    }
    render();
}

static int snake_tick_ms(void) {
    /* INTRO / PAUSED / DEAD / WON tick at 50 ms so the splash + flash
     * animations stay smooth. Gameplay tick honours S.tick_ms. */
    if (S.phase == PHASE_INTRO || S.phase == PHASE_PAUSED || S.phase == PHASE_DEAD ||
        S.phase == PHASE_WON) {
        return 50;
    }
    return S.tick_ms;
}

/* ─── Module registration ──────────────────────────────────────────── */

const struct game_module snake_module = {
    .name = "Snake",
    .glyph = glyph_S,
    .glyph_color = GAME_COLOR(0x00FF00),
    .enter = snake_enter,
    .exit = snake_exit,
    .tick = snake_tick,
    .input = snake_input,
    .tick_ms = snake_tick_ms,
};

#endif /* CONFIG_AURORAKEY_GAMES && CONFIG_AURORAKEY_GAME_SNAKE */
