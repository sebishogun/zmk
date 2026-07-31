/*
 * AuroraKey games — runtime orchestrator (v2, multi-game).
 *
 * Subscribes to layer-state-changed and position-state-changed.
 * On entering CONFIG_AURORAKEY_GAME_LAYER, dispatches enter / tick /
 * input to the ACTIVE game module. On leaving, dispatches exit and
 * releases the per-key Studio overlay so normal layer rendering resumes.
 *
 * v2: multiple games can be compiled in at once. Each game exports a
 * `const struct game_module` (vtable). The runtime keeps a registry of
 * the compiled-in modules and one `active` index. The reserved CYCLE
 * key (GKEY_CYCLE=72) advances to the next game: exit current → clear →
 * brief name-glyph splash → enter next. EXIT (53) leaves the layer.
 *
 * Everything renders on the physical keyboard's per-key RGB only — the
 * Studio overlay on the game layer, fanned out to the peripheral over
 * split-bt. There is no off-keyboard preview.
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(aurorakey_games, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/rgb_underglow_layer.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&                   \
    IS_ENABLED(CONFIG_EXPERIMENTAL_RGB_LAYER) && IS_ENABLED(CONFIG_AURORAKEY_GAME_FULL_SCREEN)
#include <zmk/split/central.h>
#define GAME_FANOUT_TO_PERIPHERAL 1
#else
#define GAME_FANOUT_TO_PERIPHERAL 0
#endif

#include "game_board.h"
#include "game_render.h"
#include "game_runtime.h"

#if IS_ENABLED(CONFIG_AURORAKEY_GAMES)

#if !IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE) && !IS_ENABLED(CONFIG_AURORAKEY_GAME_CONWAY) &&         \
    !IS_ENABLED(CONFIG_AURORAKEY_GAME_CONNECT4)
#error "CONFIG_AURORAKEY_GAMES=y requires at least one game module enabled"
#endif

#define GAME_LAYER CONFIG_AURORAKEY_GAME_LAYER
#define EXIT_POS CONFIG_AURORAKEY_GAME_EXIT_POSITION

/* Duration of the name-glyph flash shown when cycling games. */
#define SPLASH_MS 700

static bool game_active = false;
static uint32_t game_layer_id = 0;

/* Per-key cache of the colour we last pushed over split-bt. Re-painting
 * the same colour is a no-op for the user and wasteful on the BLE link:
 * at 80 cells × 5 Hz = 400 writes/sec the central's split-bt msgq (cap
 * ~16) fills, the EAGAIN handler evicts the oldest write, and most of
 * the frame's pixels never reach RH. With this dirty cache we only fan
 * out cells whose colour actually changed. */
static uint32_t last_pushed[ZMK_KEYMAP_LEN];
/* Sentinel no real packed colour can equal — seeding the cache with it
 * forces the first paint of each cell to create an explicit overlay
 * entry (even for OFF), so stock DT underglow can't bleed through. */
#define CACHE_UNKNOWN 0xFFFFFFFEu
static void cache_reset_unknown(void) {
    for (int i = 0; i < ZMK_KEYMAP_LEN; i++) {
        last_pushed[i] = CACHE_UNKNOWN;
    }
}

/* ─── Public paint API ─────────────────────────────────────────────── */

static void paint_one(int pos, uint32_t color) {
    if (pos < 0 || pos >= ZMK_KEYMAP_LEN) {
        return;
    }
    if (last_pushed[pos] == color) {
        return;
    }
    /* Local call takes the layer ID: on the central, layer_id_to_index()
     * resolves it through keymap_layer_orders. */
    zmk_rgb_underglow_layer_stage_set(game_layer_id, (uint32_t)pos, color);
#if GAME_FANOUT_TO_PERIPHERAL
    /* The split call takes the layer INDEX, not the ID. The peripheral
     * has no Studio, so CONFIG_ZMK_KEYMAP_LAYER_REORDERING is off there
     * and its layer_id_to_index() is `return layer_id` — it treats the
     * incoming value as a raw index (see rgb_underglow_studio.c, which
     * documents that the central forwards an already-resolved index).
     *
     * The central DOES have Studio, which selects LAYER_REORDERING, so
     * LAYER_INDEX_TO_ID is keymap_layer_orders[] and an id need not
     * equal its index. Sending game_layer_id here made the peripheral
     * stage every pixel into pending_colors[id] while it rendered
     * pending_colors[index] — so RH received the whole game and
     * displayed none of it, sitting on its stored layer colour instead.
     * That is the "peripheral just stays the current colour" bug. */
    int err = zmk_split_central_update_rgb_color((uint32_t)GAME_LAYER, (uint32_t)pos, color);
    if (err < 0) {
        /* msgq full (-EAGAIN) or transient BLE error. Leave the cache
         * stale so the next paint pass retries this cell; LH already
         * rendered correctly, RH catches up within 1–2 ticks. */
        return;
    }
#endif
    last_pushed[pos] = color;
}

void game_paint(int x, int y, uint32_t color) {
    if (!game_active) {
        return;
    }
    if (x < 0 || x >= game_board.width || y < 0 || y >= game_board.height) {
        return;
    }
    paint_one(game_board.xy_to_pos(x, y), color);
}

void game_paint_clear(void) {
    if (!game_active) {
        return;
    }
    for (int y = 0; y < game_board.height; y++) {
        for (int x = 0; x < game_board.width; x++) {
            paint_one(game_board.xy_to_pos(x, y), GAME_COLOR_OFF);
        }
    }
}

void game_paint_pos(uint32_t pos, uint32_t color) {
    if (!game_active) {
        return;
    }
    paint_one((int)pos, color);
}

void game_clear_all(void) {
    if (!game_active) {
        return;
    }
    for (int p = 0; p < ZMK_KEYMAP_LEN; p++) {
        paint_one(p, GAME_COLOR_OFF);
    }
}

/* The 12 thumb-cluster keys are NOT on the logical grid, so
 * game_paint_clear() (grid-only) never touches them — historically they
 * kept their DT-baked layer colour under the game ("colours not cleared
 * on start"). Clearing just these 12 on activation fixes that without
 * the 80-cell burst a full clear would cost on a fresh canvas. Games
 * then paint their control legend on top. */
static const uint8_t thumb_keys[] = {GKEY_LH_TL, GKEY_EXIT,  GKEY_LH_TR, GKEY_RH_TL,
                                     GKEY_RH_TM, GKEY_RH_TR, GKEY_LH_BL, GKEY_LH_BM,
                                     GKEY_LH_BR, GKEY_CYCLE, GKEY_RH_BM, GKEY_RH_BR};
static void clear_thumb(void) {
    for (size_t i = 0; i < ARRAY_SIZE(thumb_keys); i++) {
        paint_one((int)thumb_keys[i], GAME_COLOR_OFF);
    }
}

/* ─── Game registry (compiled-in modules) ──────────────────────────── */

static const struct game_module *const games[] = {
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)
    &snake_module,
#endif
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_CONWAY)
    &conway_module,
#endif
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_CONNECT4)
    &connect4_module,
#endif
};
#define N_GAMES ((int)ARRAY_SIZE(games))

/* Persisted across activations so re-entering the layer resumes the
 * last-played game. */
static int active = 0;

enum run_mode { MODE_PLAYING, MODE_SPLASH };
static enum run_mode mode = MODE_PLAYING;
static int64_t splash_started = 0;

static const struct game_module *cur(void) { return games[active]; }

/* ─── Module dispatch helpers ──────────────────────────────────────── */

static void dispatch_enter(void) {
    if (cur()->enter) {
        cur()->enter();
    }
}
static void dispatch_exit(void) {
    if (cur()->exit) {
        cur()->exit();
    }
}
static void dispatch_tick(void) {
    if (cur()->tick) {
        cur()->tick();
    }
}
static void dispatch_input(uint32_t position) {
    if (cur()->input) {
        cur()->input(position);
    }
}
static int dispatch_tick_ms(void) {
    if (cur()->tick_ms) {
        return cur()->tick_ms();
    }
    return CONFIG_AURORAKEY_GAME_TICK_MS;
}

/* Draw the active game's name glyph for the cycle splash. Both halves
 * in full-screen mode (so the switch reads on the whole board), LH-only
 * otherwise. */
static void draw_splash(void) {
    const struct game_module *g = cur();
    if (!g->glyph) {
        return;
    }
    uint32_t color = g->glyph_color ? g->glyph_color : GAME_COLOR(0xFFFFFF);
    game_draw_glyph(g->glyph, 1, 0, color);
    if (game_board.playable_x_max >= 11) {
        game_draw_glyph(g->glyph, 9, 0, color);
    }
}

/* ─── Tick timer ───────────────────────────────────────────────────── */

static void tick_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tick_work, tick_handler);

static void tick_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!game_active) {
        return;
    }
    if (mode == MODE_SPLASH) {
        if (k_uptime_get() - splash_started >= SPLASH_MS) {
            game_clear_all();
            mode = MODE_PLAYING;
            dispatch_enter();
            int next = dispatch_tick_ms();
            if (next < 20) {
                next = 20;
            }
            k_work_reschedule(&tick_work, K_MSEC(next));
        } else {
            k_work_reschedule(&tick_work, K_MSEC(50));
        }
        return;
    }
    dispatch_tick();
    int next = dispatch_tick_ms();
    if (next < 20) {
        next = 20; /* hard floor — don't peg the work queue */
    }
    k_work_reschedule(&tick_work, K_MSEC(next));
}

/* ─── Peripheral clear, repeated ───────────────────────────────────── */

#if GAME_FANOUT_TO_PERIPHERAL
/* The clear-layer fanout is a single fire-and-forget GATT write with no
 * acknowledgement, sent at the exact moment the split link is busiest —
 * the last gameplay frame has just queued up to 80 colour writes ahead of
 * it. zmk_split_central_rgb_clear_layer() returns -EAGAIN when the msgq
 * is full, and deactivate() used to discard that. One drop and the
 * peripheral keeps the finished game on screen indefinitely: nothing
 * repaints layer 5 until the user enters it again, and the central's
 * dirty cache has already been reset so it does not know RH is stale.
 * That is the "sometimes it just doesn't clear" report.
 *
 * paint_one solves the same problem by leaving the cache dirty so the
 * next frame retries. There is no next frame after exit, so retry
 * explicitly: re-send a few times, spaced far enough apart that the
 * queue has drained. Each send is 5 bytes, so this is cheap insurance
 * rather than an optimisation worth tuning.
 */
#define CLEAR_RETRIES 4
#define CLEAR_RETRY_MS 60

static int clear_retries_left;

static void clear_retry_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(clear_retry_work, clear_retry_handler);

static void clear_retry_handler(struct k_work *work) {
    ARG_UNUSED(work);
    int err = zmk_split_central_rgb_clear_layer((uint32_t)GAME_LAYER);
    if (err < 0) {
        LOG_DBG("peripheral clear retry failed (%d), %d left", err, clear_retries_left);
    }
    if (--clear_retries_left > 0) {
        k_work_reschedule(&clear_retry_work, K_MSEC(CLEAR_RETRY_MS));
    }
}

static void clear_peripheral_repeatedly(void) {
    clear_retries_left = CLEAR_RETRIES;
    k_work_reschedule(&clear_retry_work, K_NO_WAIT);
}
#endif /* GAME_FANOUT_TO_PERIPHERAL */

/* ─── Cycle to the next game ────────────────────────────────────────── */

static void cycle_to_next(void) {
    if (N_GAMES <= 1) {
        return; /* nothing to cycle to */
    }
    dispatch_exit();
    game_clear_all();
    active = (active + 1) % N_GAMES;
    mode = MODE_SPLASH;
    splash_started = k_uptime_get();
    LOG_INF("game cycle -> %s", cur()->name);
    draw_splash();
    k_work_reschedule(&tick_work, K_MSEC(50));
}

/* ─── Activation / deactivation ───────────────────────────────────────── */

static void activate(void) {
    if (game_active) {
        return;
    }
    if (N_GAMES <= 0) {
        return;
    }
    game_layer_id = (uint32_t)zmk_keymap_layer_index_to_id((zmk_keymap_layer_index_t)GAME_LAYER);
    game_active = true;
    mode = MODE_PLAYING;
    /* Leave the cache CACHE_UNKNOWN so the first paint of each grid cell
     * creates an explicit overlay entry (the game warms the grid up).
     * Clear the 12 thumb keys now (they're off-grid) so they don't bleed
     * the layer's colour before the game paints its control legend. */
    cache_reset_unknown();
    clear_thumb();
    LOG_INF("game enter: %s (layer index=%d, id=%u)", cur()->name, (int)GAME_LAYER, game_layer_id);
    dispatch_enter();
    /* First tick immediately so the initial frame shows on entry. */
    k_work_reschedule(&tick_work, K_NO_WAIT);
}

static void deactivate(void) {
    if (!game_active) {
        return;
    }
    LOG_INF("game exit");
    /* Only the active game is "entered" in PLAYING mode; in SPLASH we've
     * already exited the previous game and not yet entered the next, so
     * don't call exit() on a module that never entered. */
    if (mode == MODE_PLAYING) {
        dispatch_exit();
    }
    game_active = false;
    mode = MODE_PLAYING;
    k_work_cancel_delayable(&tick_work);
    /* Release every pixel we painted — the layer falls back to its
     * configured colours. One clear-layer fanout so RH stops too. */
    zmk_rgb_underglow_layer_clear(game_layer_id);
#if GAME_FANOUT_TO_PERIPHERAL
    /* Index, not id — same split contract as paint_one above. With the
     * id, the peripheral cleared some other layer's buffer and left the
     * game's pixels standing on RH after exit. Repeated because a single
     * write can be dropped and nothing would ever repaint RH. */
    clear_peripheral_repeatedly();
#endif
    cache_reset_unknown();
}

/* ─── Event listeners ─────────────────────────────────────────────── */

static int on_layer_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    int top = zmk_keymap_highest_layer_active();
    if (top == GAME_LAYER) {
        activate();
    } else if (game_active) {
        deactivate();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(aurorakey_game_layer, on_layer_state);
ZMK_SUBSCRIPTION(aurorakey_game_layer, zmk_layer_state_changed);

static int on_position_state(const zmk_event_t *eh) {
    if (!game_active) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    /* Releases pass through silently — gameplay is press-driven. */
    if (!ev->state) {
        return ZMK_EV_EVENT_HANDLED;
    }
    /* Exit key: switch back to layer 0; the layer listener runs cleanup. */
    if ((int)ev->position == EXIT_POS) {
        zmk_keymap_layer_to(zmk_keymap_layer_index_to_id(0), false);
        return ZMK_EV_EVENT_HANDLED;
    }
    /* Cycle key: next game (no-op with a single game compiled in). */
    if (ev->position == GKEY_CYCLE) {
        cycle_to_next();
        return ZMK_EV_EVENT_HANDLED;
    }
    /* Ignore gameplay input during the cycle splash. */
    if (mode == MODE_SPLASH) {
        return ZMK_EV_EVENT_HANDLED;
    }
    /* Forward the rest of the thumb cluster to the active game. */
    switch (ev->position) {
    case GKEY_LH_TL:
    case GKEY_LH_TR:
    case GKEY_RH_TL:
    case GKEY_RH_TM:
    case GKEY_RH_TR:
    case GKEY_LH_BL:
    case GKEY_LH_BM:
    case GKEY_LH_BR:
    case GKEY_RH_BM:
    case GKEY_RH_BR:
        dispatch_input(ev->position);
        return ZMK_EV_EVENT_HANDLED;
    default:
        /* Every other key on the game layer is silently consumed so the
         * user can't accidentally type mid-game. */
        return ZMK_EV_EVENT_HANDLED;
    }
}

ZMK_LISTENER(aurorakey_game_input, on_position_state);
ZMK_SUBSCRIPTION(aurorakey_game_input, zmk_position_state_changed);

/* Force-deactivate on IDLE / SLEEP so the tick loop doesn't fight the
 * activity manager during the sleep transition, and we cleanly cancel
 * work + release the overlay before power-off. */
static int on_activity_state(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (game_active && (ev->state == ZMK_ACTIVITY_IDLE || ev->state == ZMK_ACTIVITY_SLEEP)) {
        LOG_INF("game force-deactivate on activity=%d", (int)ev->state);
        deactivate();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(aurorakey_game_activity, on_activity_state);
ZMK_SUBSCRIPTION(aurorakey_game_activity, zmk_activity_state_changed);

#endif /* CONFIG_AURORAKEY_GAMES */
