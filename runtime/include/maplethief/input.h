/* Game actions (#67, ADR 0048): what the player can tell the game, as the platform's devices
 * (gamepads, the keyboard) report it, delivered to the guest the way the on-screen key pad
 * delivers it (docs/controls/keypad.md, #66). The platform maps device input to actions; this
 * module turns actions into guest calls. It knows nothing of SDL.
 *
 *   pad actions     Up, Down, Left, Right, Jump, Attack, Slot1-4 have a key on the pad. While
 *                   the pad is live (TouchKeyPadView.touchKeyPadEnable), a press sets its
 *                   polled BOOL (onLeftKey ... onShort4Key) through the property setter
 *                   (objc_msgSend) and a release clears it, as a finger on the key does, so
 *                   SkyCanvas::Tick's priority, chords (left + jump = '1') and two-tick key
 *                   switching stay the game's own. While the pad is off they go in as their
 *                   key codes, like the other actions.
 *   key actions     Clet_HandleEvent(2 press | 3 release, code, 0) 0x5A75 through
 *                   mt_call_guest: the entry point every key pad button calls.
 *
 * Delivery never loses an event. SkyCanvas holds one pending press (+0xD8) and one pending
 * release (+0xE8) and drops a second one before its tick, and the pad's BOOLs are only seen
 * when a tick polls them. So the actions wait in a FIFO, and mt_input_turn (run at the start
 * of every UIKit turn, where the queued touches are delivered too) applies at most one step per
 * game tick, and only while both of SkyCanvas's slots are empty: one key event, or a batch of
 * BOOL changes for different keys (keys pressed together reach the tick together, like two
 * fingers). The tick is seen through CMTimer's lastFrame (0x11A4D8), which the game sets
 * before every SkyCanvas::Tick. A release always goes the way its press went (a BOOL set while
 * the pad was live is cleared even if the pad is off by then).
 *
 * Several devices may hold one action: each press counts, and the guest sees the press of the
 * first and the release of the last.
 *
 * The context: mt_input_panel() is the key pad's panel (EAGLView.statusOfTouchKeyPad, ivar
 * +92: -1 none, 0 the game pad, 1 popup menu, 3 cut scene, 5 NPC chat, 6 shop, 7 popup, 8 map,
 * 9 cash shop, 10 help), read at every turn. Where the status is -1 while the key pad still
 * shows a panel (a popup over the shop, closed by disableAlertKey, #127), it is that panel. The
 * platform's bindings switch on it (field: 0; menus and popups: everything else).
 *
 * Single guest thread: every function runs on the guest's main thread (the frontend's turn
 * hook and its event pump), never on another. Trace: MAPLETHIEF_TRACE=input. C17 that also
 * compiles as C++20; the implementation is runtime/src/input/. */
#ifndef MAPLETHIEF_INPUT_H
#define MAPLETHIEF_INPUT_H

#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

/* The actions. The numbers are not stable across versions: store names (mt_input_action_name). */
typedef enum mt_input_action {
    MT_ACTION_NONE = 0,
    /* The pad (docs/controls/keypad.md, "Panel 0"). */
    MT_ACTION_UP,     /* -1: climb, talk to an NPC, enter a portal; cursor up */
    MT_ACTION_DOWN,   /* -2: climb down; cursor down */
    MT_ACTION_LEFT,   /* -3: walk left; previous tab, minus */
    MT_ACTION_RIGHT,  /* -4: walk right; next tab, plus */
    MT_ACTION_JUMP,   /* '2' (with left / right the pad makes '1' / '3') */
    MT_ACTION_ATTACK, /* '5': attack; OK in popups */
    MT_ACTION_SLOT1,  /* '7': quick slot 1 (a new character: Red Potion) */
    MT_ACTION_SLOT2,  /* '9': quick slot 2 (a new character: Blue Potion) */
    MT_ACTION_SLOT3,  /* '0': quick slot 3 (a skill) */
    MT_ACTION_SLOT4,  /* '#': quick slot 4 (a skill) */
    /* The buttons. */
    MT_ACTION_OK,           /* -5: confirm, yes, accept, next line */
    MT_ACTION_BACK,         /* -16: back, cancel, no, refuse, close */
    MT_ACTION_MENU,         /* -16: in the field, the popup menu (items, skills, options) */
    MT_ACTION_MAP,          /* '*': the map; switch the map's view */
    MT_ACTION_CASH_SHOP,    /* -26: the cash shop */
    MT_ACTION_MINUS,        /* '4': minus, previous tab */
    MT_ACTION_PLUS,         /* '6': plus, next tab */
    MT_ACTION_REVIVE,       /* -25: revive with an item (the death popup) */
    MT_ACTION_GRADE_REVIVE, /* -35: premium revive (the death popup) */
    /* The port's own actions (#151): bound, remapped and saved like the others, but the
     * frontend handles them (platform/src/actions.h) and they never reach the guest:
     * mt_input_press ignores them. */
    MT_ACTION_FAST_FORWARD, /* held: the game runs faster (fast_forward, ADR 0087) */
    MT_ACTION_AUTO_RUN,     /* pressed: the walk goes on by itself (auto_run, ADR 0094) */
    MT_ACTION_PAGE,         /* held: up / down page a list a view at a time (ADR 0114) */
    MT_ACTION_COUNT
} mt_input_action;

/* The first of the port's own actions: [MT_ACTION_FIRST_PORT, MT_ACTION_COUNT). */
#define MT_ACTION_FIRST_PORT MT_ACTION_FAST_FORWARD

/* An action's row of the fixed table (data: the bindings of #68 and #70 name actions by it). */
typedef struct mt_input_action_info {
    const char *name;    /* "up", "jump", "slot1", "cash_shop", ... (stable) */
    const char *title;   /* "Up", "Quick slot 1", ... (for a bindings UI) */
    const char *pad_key; /* the TouchKeyPadView BOOL ("onLeftKey"), or NULL: key only */
    int32_t code;        /* the phone key code (Clet_HandleEvent); 0 for the port's actions */
} mt_input_action_info;

/* The row of `action`, or NULL for NONE and out-of-range values. */
const mt_input_action_info *mt_input_action_get_info(uint32_t action) MT_NOEXCEPT;
/* The action named `name`, or MT_ACTION_NONE. */
uint32_t mt_input_action_named(const char *name) MT_NOEXCEPT;

/* Presses / releases `action` (counted per action: see above). A release of an action that is
 * not held is ignored, and so are the port's own actions (MT_ACTION_FIRST_PORT on). Queued;
 * mt_input_turn delivers. */
void mt_input_press(uint32_t action) MT_NOEXCEPT;
void mt_input_release(uint32_t action) MT_NOEXCEPT;
/* How many presses of `action` are held. */
uint32_t mt_input_held(uint32_t action) MT_NOEXCEPT;
/* Releases every held action (queued like any release). */
void mt_input_release_all(void) MT_NOEXCEPT;

/* The key pad's panel at the last turn (EAGLView.statusOfTouchKeyPad, else the shown panel
 * where it is -1), -1 before the game has one and on screens without the key pad. */
int32_t mt_input_panel(void) MT_NOEXCEPT;
/* 1 if the pad was live at the last turn (TouchKeyPadView.touchKeyPadEnable). */
int mt_input_pad_live(void) MT_NOEXCEPT;

/* The key pad's panel container `index` (TouchKeyPadView.subviews[index], 0 the game pad ...
 * 10 help: docs/controls/keypad.md) of the pad seen at the last turn, or 0. The platform's
 * key pad hiding (#71, ADR 0052) fades panel 0 through mt_uikit_set_host_opacity. */
uint32_t mt_input_pad_panel_view(int32_t index) MT_NOEXCEPT;
/* The key pad (EAGLView.touchKeyPadView, a TouchKeyPadView) seen at the last turn, or 0. */
uint32_t mt_input_pad_view(void) MT_NOEXCEPT;

/* Delivers what is due (see above). mt_uikit_turn runs it first, before the queued touches. */
void mt_input_turn(mt_cpu *cpu) MT_NOEXCEPT;

/* Where the game keeps what this module reads and calls. The defaults are the game's
 * (docs/controls/keypad.md, docs/re/program-flow.md); tests point them at a synthetic guest. */
typedef struct mt_input_config {
    uint32_t handle_event;  /* Clet_HandleEvent(type, code, arg): 0x5A75 */
    uint32_t canvas_global; /* the SkyCanvas pointer: 0x11A950 */
    uint32_t last_frame;    /* CMTimer's lastFrame (int64, ms): 0x11A4D8 */
} mt_input_config;

mt_input_config mt_input_config_default(void) MT_NOEXCEPT;
/* Sets the configuration (NULL: the default) and forgets everything: queue, holds, the guest
 * objects seen. */
void mt_input_configure(const mt_input_config *config) MT_NOEXCEPT;

typedef struct mt_input_stats {
    uint32_t queued;     /* events waiting */
    uint64_t presses;    /* guest presses delivered (BOOL set or key press) */
    uint64_t releases;   /* guest releases delivered */
    uint64_t pad_edges;  /* of them, through the pad's BOOLs */
    uint64_t key_events; /* of them, through Clet_HandleEvent */
    uint64_t steps;      /* delivery steps (at most one per tick) */
    uint64_t ticks;      /* game ticks seen */
    uint64_t waits_busy; /* turns a due step waited for SkyCanvas's slots */
} mt_input_stats;

mt_input_stats mt_input_get_stats(void) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_INPUT_H */
