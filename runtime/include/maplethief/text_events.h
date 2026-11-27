/* Text events (#391, ADR 0187; #392, ADR 0191; docs/re/speech.md): the game's dialogue, notify
 * lines and popups as the host wants to hear of them (the speech, platform/src/speech_text.h),
 * read where the game has just set them. It knows nothing of SDL.
 *
 *   page       CChatBox::FirstPage 0x34CD1 (page 0) and CChatBox::NextPage 0x34A05 (page + 1),
 *              each (box), wrapped (mt_hook_wrap, ADR 0069): after the original returns true,
 *              the page's wrapped lines (+0x18, their count +0x1C) joined into one text. The
 *              speaker is the one CChatBox::paint 0x32BBD draws: the page's CMsg kind (+0x0C,
 *              copied from the CMsg's +0x08) 0 is the hero (ScriptExtra "CChatBox.paint.1");
 *              else the NPC the box talks for, CStateGame +0xAC (the box's state +0x48): its
 *              name in the NPC table, or, when it is -1, the boss in the mob list (its name
 *              +0x9C).
 *   answer     after the last page's event, when the box asks for an answer (mode +0x08: 1 and
 *              3 Accept / Refuse, `text` empty; 2 a list of rows, +0x20 CSelItem: their
 *              Scripts keys +0x04 localized, one row per line).
 *   scene box  CStateGame::Paint 0x87501 (this), wrapped: after the frame, while the state is
 *              still the engine's, the scene's speech box (+0x1E9 on) when it opens or its first
 *              line (+0x4C8 of +0x4D4 lines at +0x4D0) moves: the two lines drawn, joined. The
 *              speaker is CChatBox::drawChatBox 0x35189's (+0x4B4): -1 the hero
 *              ("CChatBox.drawChatBox.1"), -2 the boss in the mob list, -0x40 and -0x42 to
 *              -0x45 the bosses of those mob ids ("CChatBox.drawChatBox.2" to ".6"), 0 and more
 *              an NPC id.
 *   notice     CNotifyTextList::Add 0x78339 (list, text, colour), wrapped (#392): a notify line,
 *              its text as the caller passed it (a NUL-ended UTF-8 string), when the list keeps
 *              lines (its most entries +0x0C is 1 or more; else Add adds nothing). value: the
 *              list, 0 CStateGame +0x4C (pick-ups, meso, EXP, under the compass), 1 +0x50, 2
 *              +0x54 (the map's name; both at the top centre), -1 another; index: the colour
 *              (0xRRGGBB).
 *   popup      Popup_Open 0x3EAA1 (mgr, id, text, kind), wrapped (#392): after it, the popup
 *              (the CMPopup at *0x11A96C): its message's wrapped lines (+0x08, count +0x0C)
 *              joined, `text`; index the id (0x108C64), value the kind (+0x10), which says
 *              the buttons: 0 none (a tap is OK); 1, 2 and 5 Yes / No; 3 and 6 OK; 4 OK and
 *              Cancel with an amount (+0x18). The buttons are images: the CMPopup's labels
 *              (+0x70, +0x74) point at freed buffers and are not read. The death popup (0x68)
 *              has no message: its four choices are images the field draws.
 *
 * Everything is read, nothing is written and no guest function is called: every pointer is
 * checked against the image's segments and the committed heap first (an event with a bad one
 * is not sent, and counts as ignored), and the localized names come from the bundle's
 * .strings on the host side (a text pack's first). Text from the scripts and the .strings is
 * UTF-8; a table name that is not (Korean CP949) is converted. Every string an event carries is
 * valid UTF-8.
 *
 * Single guest thread: the listeners are called on the guest thread, inside the guest's call,
 * with strings that live until they return. They must not call back into the guest. Trace:
 * MAPLETHIEF_TRACE=text. C17 that also compiles as C++20; the implementation is
 * runtime/src/events/text_events.cpp. */
#ifndef MAPLETHIEF_TEXT_EVENTS_H
#define MAPLETHIEF_TEXT_EVENTS_H

#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

typedef enum mt_text_event_kind {
    MT_TEXT_EVENT_PAGE = 1,      /* an NPC dialogue page: index the page, count the pages */
    MT_TEXT_EVENT_ANSWER = 2,    /* the box asks for an answer: value its mode (1, 2, 3) */
    MT_TEXT_EVENT_SCENE_BOX = 3, /* a scene's speech box: index its first line, count its lines */
    MT_TEXT_EVENT_NOTICE = 4,    /* a notify line (#392): value the list, index its colour */
    MT_TEXT_EVENT_POPUP = 5,     /* a popup (#392): index its id, value its kind */
} mt_text_event_kind;

typedef enum mt_text_speaker {
    MT_TEXT_SPEAKER_NONE = 0, /* not known (speaker "") */
    MT_TEXT_SPEAKER_HERO = 1, /* the hero; id -1 */
    MT_TEXT_SPEAKER_NPC = 2,  /* id: the NPC's id (the NPC table's column 0) */
    MT_TEXT_SPEAKER_MOB = 3,  /* a boss; id: its mob id */
} mt_text_speaker;

typedef struct mt_text_event {
    uint32_t kind;         /* mt_text_event_kind */
    uint32_t speaker_kind; /* mt_text_speaker */
    int32_t speaker_id;    /* see mt_text_speaker */
    int32_t index;         /* see the kind */
    int32_t count;         /* see the kind */
    int32_t value;         /* see the kind, else 0 */
    const char *speaker;   /* the name the game draws for the speaker (UTF-8), "" if none */
    const char *text;      /* UTF-8; an answer's rows one per line, "" for Accept / Refuse */
} mt_text_event;

typedef void (*mt_text_event_fn)(void *user, const mt_text_event *event);

/* At most MT_TEXT_EVENTS_MAX_LISTENERS; adding one twice is one. Returns 0, or -1 if full.
 * Kept across processes. */
#define MT_TEXT_EVENTS_MAX_LISTENERS 4
int mt_text_events_add_listener(mt_text_event_fn fn, void *user) MT_NOEXCEPT;
void mt_text_events_remove_listener(mt_text_event_fn fn, void *user) MT_NOEXCEPT;

/* Registers the wrappers (FirstPage, NextPage, CStateGame::Paint, CNotifyTextList::Add,
 * Popup_Open), on or off, and turns them on or off later: off, the game runs exactly as without
 * them. Idempotent; any thread. */
void mt_text_events_install(int enabled) MT_NOEXCEPT;
/* Forgets the scene box last seen (a save state is restored, a new guest process). */
void mt_text_events_reset(void) MT_NOEXCEPT;

typedef struct mt_text_events_stats {
    uint64_t entries;     /* wrapped calls */
    uint64_t pages;       /* page events */
    uint64_t answers;     /* answer events */
    uint64_t scene_boxes; /* scene box events */
    uint64_t notices;     /* notice events (#392) */
    uint64_t popups;      /* popup events (#392) */
    uint64_t ignored;     /* a page, box, line or popup with a pointer that is not guest memory */
} mt_text_events_stats;

mt_text_events_stats mt_text_events_get_stats(void) MT_NOEXCEPT;
void mt_text_events_reset_stats(void) MT_NOEXCEPT;

/* The guest facts (docs/re/speech.md). */
#define MT_TEXT_FN_CHAT_FIRST_PAGE 0x34CD1u /* = MT_HOOK_FN_CHAT_BOX_FIRST_PAGE */
#define MT_TEXT_FN_CHAT_NEXT_PAGE 0x34A05u  /* = MT_HOOK_FN_CHAT_BOX_NEXT_PAGE */
#define MT_TEXT_NPC_TABLE 0x11A988u         /* the NPC table's bytes (pointer) */
#define MT_TEXT_MOB_LIST 0x11A9ECu          /* the mobs (pointer to a list: count +4, items +8) */
#define MT_TEXT_FN_NOTIFY_ADD 0x78339u      /* = MT_HOOK_FN_NOTIFY_TEXT_ADD */
#define MT_TEXT_FN_POPUP_OPEN 0x3EAA1u      /* = MT_HOOK_FN_POPUP_OPEN */
#define MT_TEXT_POPUP 0x11A96Cu             /* the CMPopup (pointer) */
#define MT_TEXT_POPUP_ID 0x108C64u          /* the open popup's id, -1 none */
#define MT_TEXT_POPUP_DEATH 0x68            /* the death popup's id */

MT_EXTERN_C_END

#endif /* MAPLETHIEF_TEXT_EVENTS_H */
