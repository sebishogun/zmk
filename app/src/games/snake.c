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
#include "game_runtime.h"

#if IS_ENABLED(CONFIG_AURORAKEY_GAMES) && IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)

#define SNAKE_MAX_LEN 80 /* covers the entire Glove80 grid */
#define DIR_BUFFER 4

#define KEY_LH_START 52
#define KEY_LH_RESET 54
#define KEY_RH_UP 56
#define KEY_RH_RIGHT 57
#define KEY_RH_LEFT 72
#define KEY_RH_DOWN 73

/* Flash duration on death / win. Pure cosmetic — gameplay is paused. */
#define SNAKE_DEATH_FLASH_MS 1500
#define SNAKE_WIN_FLASH_MS 4000

/* Speedup: tick *= (100 - ACCEL_PCT) / 100 per food, floored at MIN_MS.
 * Default ACCEL_PCT=0 keeps a steady pace, which is what most users
 * want; users who like progressively-harder Snake tick it up. */
#define ACCEL_PCT CONFIG_AURORAKEY_GAME_SNAKE_ACCEL_PCT
#define MIN_TICK_MS 60

enum snake_phase {
    PHASE_PLAYING,
    PHASE_PAUSED,
    PHASE_DEAD,
    PHASE_WON,
};

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
};

static struct snake S;

/* ─── Helpers ──────────────────────────────────────────────────────── */

static int wrap(int v, int max) {
    /* Positive-modulo so callers can pass v < 0. */
    return ((v % max) + max) % max;
}

static int playable_cell_count(void) {
    int n = 0;
    for (int y = 0; y < game_board.height; y++) {
        for (int x = 0; x < game_board.width; x++) {
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
    for (int i = 0; i < 64; i++) {
        int x = (int)(sys_rand32_get() % (uint32_t)game_board.width);
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
        for (int x = 0; x < game_board.width; x++) {
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
    return v;
#else
    return CONFIG_AURORAKEY_GAME_TICK_MS;
#endif
}

static void snake_reset(void) {
    memset(&S, 0, sizeof(S));
    S.len = 3;
    /* Spawn near the centre of the playable area on a non-wall row. */
    int cx = game_board.width / 2;
    int cy = game_board.height / 2;
    /* Walk left until we find 3 consecutive non-wall cells for the
     * starting body. If the board is degenerate (no such run) we fall
     * back to the first non-wall cell. */
    int sx = cx, sy = cy;
    for (int probe = 0; probe < game_board.width; probe++) {
        int x = wrap(cx - probe, game_board.width);
        bool ok = true;
        for (int k = 0; k < 3; k++) {
            int xk = wrap(x + k, game_board.width);
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
    S.body[0].x = (int8_t)wrap(sx + 2, game_board.width);
    S.body[0].y = (int8_t)sy;
    S.body[1].x = (int8_t)wrap(sx + 1, game_board.width);
    S.body[1].y = (int8_t)sy;
    S.body[2].x = (int8_t)sx;
    S.body[2].y = (int8_t)sy;
    S.dx = 1;
    S.dy = 0;
    S.dir_buffer_len = 0;
    S.tick_ms = snake_default_tick_ms();
    S.phase = PHASE_PLAYING;
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
}

static void step_playing(void) {
    consume_one_buffered_direction();
    int nx = S.body[0].x + S.dx;
    int ny = S.body[0].y + S.dy;
    bool wrap_enabled = IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE_WRAP);
    if (wrap_enabled) {
        nx = wrap(nx, game_board.width);
        ny = wrap(ny, game_board.height);
    } else {
        if (nx < 0 || nx >= game_board.width || ny < 0 || ny >= game_board.height) {
            enter_phase(PHASE_DEAD);
            return;
        }
    }
    /* Step over wall cells: continue in the same direction, with a
     * bounded safety so a fully-walled row doesn't hang the tick. */
    int safety = game_board.width + game_board.height;
    while (game_board_cell_is_wall(nx, ny) && safety-- > 0) {
        nx += S.dx;
        ny += S.dy;
        if (wrap_enabled) {
            nx = wrap(nx, game_board.width);
            ny = wrap(ny, game_board.height);
        } else if (nx < 0 || nx >= game_board.width || ny < 0 || ny >= game_board.height) {
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

static void render(void) {
    game_paint_clear();
    /* Food: red. Painted before the body so head colour wins if the
     * head ever overlaps food (it shouldn't — we transition to ate
     * before render — but defensive). */
    if (S.phase == PHASE_PLAYING || S.phase == PHASE_PAUSED) {
        game_paint(S.food.x, S.food.y, GAME_COLOR(0xFF0000));
    }
    /* Body. */
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
        int64_t age = k_uptime_get() - S.phase_started_ms;
        int phase = (int)((age / 25) % 80);
        int amp = phase < 40 ? phase : (80 - phase); /* 0..40..0 */
        uint8_t b = (uint8_t)(amp * 4);              /* up to ~160 */
        head_color = GAME_COLOR((uint32_t)b << 8);   /* dim green pulse */
        body_color = GAME_COLOR(((uint32_t)(b / 2)) << 8);
        break;
    }
    case PHASE_PLAYING:
    default:
        head_color = GAME_COLOR(0x00FF00);
        body_color = GAME_COLOR(0x004000);
        break;
    }
    /* Render tail-first so the head paints last (wins on overlap). */
    for (int i = S.len - 1; i >= 0; i--) {
        uint32_t c = (i == 0) ? head_color : body_color;
        game_paint(S.body[i].x, S.body[i].y, c);
    }
}

/* ─── Game module hooks ────────────────────────────────────────────── */

void snake_init(void) { /* no-op; state is reset on enter */ }

void snake_enter(void) {
    snake_reset();
    render();
}

void snake_exit(void) { /* Nothing to do; runtime clears the layer's overlay. */ }

void snake_tick(void) {
    int64_t now = k_uptime_get();
    switch (S.phase) {
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

void snake_input(uint32_t position) {
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
        if (S.phase == PHASE_PLAYING) {
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

int snake_tick_ms(void) {
    /* DEAD / WON freeze: render at 50 ms for the flash animation,
     * gameplay tick honour S.tick_ms otherwise. */
    if (S.phase == PHASE_DEAD || S.phase == PHASE_WON || S.phase == PHASE_PAUSED) {
        return 50;
    }
    return S.tick_ms;
}

#endif /* CONFIG_AURORAKEY_GAMES && CONFIG_AURORAKEY_GAME_SNAKE */
