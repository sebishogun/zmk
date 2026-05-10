/*
 * AuroraKey games — runtime orchestrator.
 *
 * Subscribes to layer-state-changed and position-state-changed.
 * On entering CONFIG_AURORAKEY_GAME_LAYER, dispatches enter / tick /
 * input to the active game module. On leaving, dispatches exit and
 * releases the per-key Studio overlay so normal layer rendering
 * resumes.
 *
 * Architecture is single-game-active-at-a-time: only one module's
 * Kconfig should be set per build. If multiple are set the runtime
 * picks the first compiled-in one (Snake → Conway → Connect4) — the
 * editor's codegen normally enforces single-select but this falls
 * back gracefully if a hand-edited config slips through.
 *
 * Re-renders every tick rather than tracking dirty regions: the
 * board has 80 keys max, paint cost is negligible (~80 atomic stores
 * + a single split-bt fanout the renderer batches), and re-render
 * recovers from any concurrent USB Studio writes that may have
 * scribbled on the game layer mid-frame.
 *
 * Copyright (c) 2026 The ZMK Contributors / AuroraKey
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(aurorakey_games, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/rgb_underglow_layer.h>

#include "game_board.h"
#include "game_runtime.h"

#if IS_ENABLED(CONFIG_AURORAKEY_GAMES)

#define GAME_LAYER CONFIG_AURORAKEY_GAME_LAYER
#define EXIT_POS CONFIG_AURORAKEY_GAME_EXIT_POSITION

/* Glove80 thumb-cluster matrix positions used as game inputs. */
#define KEY_LH_START 52
#define KEY_LH_RESET 54
#define KEY_RH_UP 56
#define KEY_RH_RIGHT 57
#define KEY_RH_LEFT 72
#define KEY_RH_DOWN 73

static bool game_active = false;
static uint32_t game_layer_id = 0;

/* ─── Public paint API ─────────────────────────────────────────────── */

void game_paint(int x, int y, uint32_t color) {
    if (!game_active) {
        return;
    }
    if (x < 0 || x >= game_board.width || y < 0 || y >= game_board.height) {
        return;
    }
    int pos = game_board.xy_to_pos(x, y);
    if (pos < 0 || pos >= ZMK_KEYMAP_LEN) {
        return;
    }
    zmk_rgb_underglow_layer_stage_set(game_layer_id, (uint32_t)pos, color);
}

void game_paint_clear(void) {
    if (!game_active) {
        return;
    }
    for (int y = 0; y < game_board.height; y++) {
        for (int x = 0; x < game_board.width; x++) {
            int pos = game_board.xy_to_pos(x, y);
            if (pos >= 0 && pos < ZMK_KEYMAP_LEN) {
                zmk_rgb_underglow_layer_stage_set(game_layer_id, (uint32_t)pos, GAME_COLOR_OFF);
            }
        }
    }
}

/* ─── Module dispatch helpers ──────────────────────────────────────── */

static void dispatch_enter(void) {
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)
    snake_enter();
#endif
}

static void dispatch_exit(void) {
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)
    snake_exit();
#endif
}

static void dispatch_tick(void) {
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)
    snake_tick();
#endif
}

static void dispatch_input(uint32_t position) {
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)
    snake_input(position);
#endif
}

static int dispatch_tick_ms(void) {
#if IS_ENABLED(CONFIG_AURORAKEY_GAME_SNAKE)
    return snake_tick_ms();
#else
    return CONFIG_AURORAKEY_GAME_TICK_MS;
#endif
}

/* ─── Tick timer ───────────────────────────────────────────────────── */

static void tick_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tick_work, tick_handler);

static void tick_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!game_active) {
        return;
    }
    dispatch_tick();
    int next = dispatch_tick_ms();
    if (next < 20) {
        next = 20; /* hard floor — don't peg the work queue */
    }
    k_work_reschedule(&tick_work, K_MSEC(next));
}

/* ─── Activation / deactivation ───────────────────────────────────── */

static void activate(void) {
    if (game_active) {
        return;
    }
    game_layer_id = (uint32_t)zmk_keymap_layer_index_to_id((zmk_keymap_layer_index_t)GAME_LAYER);
    game_active = true;
    LOG_INF("game enter (layer index=%d, layer_id=%u)", (int)GAME_LAYER, game_layer_id);
    dispatch_enter();
    /* First tick fires immediately so the user sees the initial frame
     * the moment they enter the layer; subsequent ticks honour the
     * module's requested interval. */
    k_work_reschedule(&tick_work, K_NO_WAIT);
}

static void deactivate(void) {
    if (!game_active) {
        return;
    }
    LOG_INF("game exit");
    dispatch_exit();
    game_active = false;
    k_work_cancel_delayable(&tick_work);
    /* Release every pixel we painted — the game layer falls back to
     * whatever the editor configured (or the layer default). */
    zmk_rgb_underglow_layer_clear(game_layer_id);
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
    /* Releases pass through silently — gameplay is press-driven; we
     * still want releases to clear any pressed-key state the kernel
     * tracks so we don't leave anything stuck after exiting the
     * layer. */
    if (!ev->state) {
        return ZMK_EV_EVENT_HANDLED;
    }
    /* Exit key: switch the keymap back to layer 0 (base). The layer-
     * state listener picks up the change and runs game_exit cleanup. */
    if ((int)ev->position == EXIT_POS) {
        zmk_keymap_layer_to(zmk_keymap_layer_index_to_id(0), false);
        return ZMK_EV_EVENT_HANDLED;
    }
    /* Game inputs: thumb-cluster keys handled by the active module. */
    switch (ev->position) {
    case KEY_LH_START:
    case KEY_LH_RESET:
    case KEY_RH_UP:
    case KEY_RH_DOWN:
    case KEY_RH_LEFT:
    case KEY_RH_RIGHT:
        dispatch_input(ev->position);
        return ZMK_EV_EVENT_HANDLED;
    default:
        /* Every other key on the game layer is silently consumed so
         * the user can't accidentally type letters mid-snake. The
         * dedicated layer + intercept-all is the production UX
         * contract: a game layer is for the game only. */
        return ZMK_EV_EVENT_HANDLED;
    }
}

ZMK_LISTENER(aurorakey_game_input, on_position_state);
ZMK_SUBSCRIPTION(aurorakey_game_input, zmk_position_state_changed);

#endif /* CONFIG_AURORAKEY_GAMES */
