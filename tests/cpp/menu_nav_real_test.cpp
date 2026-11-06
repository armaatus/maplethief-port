// Controller navigation in the real game (#69, ADR 0051): every menu, screen and popup done
// with a virtual gamepad only, no taps. The recompiled game boots from work/ as MapleThief does
// with --fixed-step 1 (the virtual clock, 1 ms per run-loop turn) on a data directory the
// caller gives; from the run loop's turn hook the test drives the frontend's own input layer:
// an SDL3 virtual Xbox 360 pad (tests/cpp/virtual_pad.h) -> Gamepads -> Actions -> Focus (the
// UIKit screens' focus, focus.h) or the runtime's actions (maplethief/input.h). The checks read
// the uikit trace (the button actions that ran, "uikit: action -[Class selector:]"), the save
// files and guest memory. Local only: it needs work/gen (python -m recomp all), the slice and
// the extracted bundle, and exits 77 (skipped) without them.
//
//   menu_nav_real_test MODE DATA_DIR OUT_DIR [STEP...]
//
// MODE is a flow (main_menu, terms, early, seed, create, select, delete, game; tutorial, hunt,
// shops: #127, past the tutorial) or "explore", which runs the STEPs "T:press BUTTON [HOLD]",
// "T:repeat BUTTON EVERY UNTIL [HOLD]", "T:stick AXIS VALUE HOLD", "T:trigger AXIS HOLD",
// "T:shot NAME", "T:state", "T:hero [EVERY UNTIL]", "T:mobs", "T:shop", "T:widgets" and the
// tasks "T:load" and "T:walkto X" (SDL's gamepad names: a, b, x, y, back, start, leftshoulder,
// dpup, ...; axes leftx, lefty, lefttrigger, righttrigger) for finding a way through the game.
// Shots (OUT_DIR/NAME.png) are for looking only: they stay in the test output directory and are
// never committed. The flows are in run_mode below, and platform/menu_nav_test.cmake runs them
// in order on one data directory (the character flows; tutorial then hunt for the save shops
// loads). The driver (the run, the tasks, the tutorial's way, the boot) is game_driver.h.
#include <SDL3/SDL.h>

#include <cstdio>
#include <string>

#if defined(MT_HAVE_GENERATED_CODE)

#include "game_driver.h"

namespace {

using namespace game_driver;

// --- explore ----------------------------------------------------------------------------------

bool explore(Run &r, int argc, char **argv) {
    attach_pad(r);
    double end = 1;
    for (int i = 0; i < argc; i++) {
        const std::string step = argv[i];
        const auto colon = step.find(':');
        if (colon == std::string::npos) {
            return false;
        }
        const double t = std::atof(step.substr(0, colon).c_str());
        end = std::max(end, t + 1);
        char what[32] = "", name[64] = "";
        double v1 = 0, v2 = 0, v3 = 0.1;
        const int n = std::sscanf(step.c_str() + colon + 1, "%31s %63s %lf %lf %lf", what, name,
                                  &v1, &v2, &v3);
        const std::string w = what;
        if (w == "press" && n >= 2) {
            const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
            press(r, t, b, n >= 3 ? v1 : 0.1);
        } else if (w == "repeat" && n >= 4) { // "T:repeat BUTTON EVERY UNTIL [HOLD]"
            const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
            for (double u = t; u <= v2 + 1e-9; u += std::max(v1, 0.2)) {
                press(r, u, b, v3);
            }
            end = std::max(end, v2 + 1);
        } else if (w == "stick" && n == 4) {
            stick(r, t, SDL_GetGamepadAxisFromString(name), static_cast<Sint16>(v1), v2);
        } else if (w == "trigger" && n == 3) {
            const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(name);
            at(r, t, [&r, a](mt_cpu *) { r.pad->trigger(a, 1.0f); });
            at(r, t + v1, [&r, a](mt_cpu *) { r.pad->trigger(a, 0.0f); });
        } else if (w == "tap" && n >= 2) { // exploring only: the flows never tap
            const float x = static_cast<float>(std::atof(name)), y = static_cast<float>(v1);
            at(r, t, [&r, x, y](mt_cpu *) {
                r.input.touch(fe::InputSource::kScripted, 0, MT_UIKIT_TOUCH_BEGAN, x, y);
            });
            at(r, t + (n >= 4 ? v2 : 0.1), [&r, x, y](mt_cpu *) {
                r.input.touch(fe::InputSource::kScripted, 0, MT_UIKIT_TOUCH_ENDED, x, y);
            });
        } else if (w == "shot" && n == 2) {
            shot(r, t, name);
        } else if (w == "state") {
            state(r, t);
        } else if (w == "load") { // "T:load": slot 0 into the game (tasks, from the start)
            then_load_slot0(r);
        } else if (w == "walkto" && n >= 2) { // "T:walkto X": a task, after the ones before
            then_walk_to(r, std::atoi(name));
        } else if (w == "shop") {
            at(r, t, [&r](mt_cpu *cpu) {
                const Shop s = shop(cpu);
                std::printf("shop at %.2f s: %X tab %d selected %d kind %d rows %d\n", r.t, s.addr,
                            s.tab, s.selected, s.kind, s.rows);
                for (i32 i = 0; i < s.rows && i < 40; i++) {
                    const ShopRow w = shop_row(cpu, s, i);
                    std::printf("  row %d: id %d price %d\n", i, w.id, w.price);
                }
                for (i32 i = 0; i < 48; i++) {
                    const EtcItem e = etc_item(cpu, hero(cpu), i);
                    if (e.id >= 0) {
                        std::printf("  etc %d: id %d type %d count %d max %d price %d\n", i, e.id,
                                    e.type, e.count, e.max, e.price);
                    }
                }
                std::fflush(stdout);
            });
        } else if (w == "mobs") {
            at(r, t, [&r](mt_cpu *cpu) {
                for (const Mob &m : mobs(cpu)) {
                    std::printf("mob at %.2f s: id %d x %d y %d HP %d\n", r.t, m.id, m.x, m.y,
                                m.hp);
                }
            });
        } else if (w == "hero") { // "T:hero [EVERY UNTIL]": the hero's line, once or repeated
            const double every = n >= 3 ? std::atof(name) : 0, until = n >= 3 ? v1 : t;
            for (double u = t; u <= until + 1e-9; u += every > 0 ? every : 1e9) {
                at(r, u, [&r](mt_cpu *cpu) {
                    const Hero h = hero(cpu);
                    std::printf("hero at %.2f s: panel %d mode %d popup %d %s\n", r.t,
                                static_cast<int>(mt_input_panel()), game_mode(cpu), popup_id(cpu),
                                hero_text(cpu, h).c_str());
                });
                end = std::max(end, u + 1);
            }
        } else if (w == "widgets") {
            at(r, t, [](mt_cpu *cpu) {
                for (u32 g = 0xF149C; g <= 0xF14C4; g += 4) {
                    const u32 wd = rd(cpu, rd(cpu, g));
                    if (wd == 0) {
                        continue;
                    }
                    std::printf("widget %X: on %d rect (%d,%d,%d,%d) items %u\n", g,
                                cpu->mem[wd + 12], (int)rd(cpu, wd + 28), (int)rd(cpu, wd + 32),
                                (int)rd(cpu, wd + 36), (int)rd(cpu, wd + 40), rd(cpu, wd + 4));
                    for (u32 i = 0; i < rd(cpu, wd + 4) && i < 64; i++) {
                        const u32 it = rd(cpu, rd(cpu, wd) + 4 * i);
                        std::printf("  item %u: rect (%d,%d,%d,%d) off (%d,%d) abs %d id %d v74 %d "
                                    "b %02x %02x %02x %02x\n",
                                    i, (int)rd(cpu, it + 0x20), (int)rd(cpu, it + 0x24),
                                    (int)rd(cpu, it + 0x28), (int)rd(cpu, it + 0x2c),
                                    (int)rd(cpu, it + 8), (int)rd(cpu, it + 12),
                                    cpu->mem[it + 0x5c], (int)rd(cpu, it + 0x60),
                                    (int)rd(cpu, it + 0x74), cpu->mem[it + 0x10],
                                    cpu->mem[it + 0x11], cpu->mem[it + 0x12], cpu->mem[it + 0x13]);
                    }
                }
                std::fflush(stdout);
            });
        } else {
            return false;
        }
    }
    finish(r, end);
    return true;
}

// --- the flows --------------------------------------------------------------------------------

// The main menu: the banner's close (B), Help, About with its links (down scrolls the text),
// Game Center, More Games, then Play.
void main_menu(Run &r) {
    attach_pad(r);
    // The menu has slid in at 3 s (#122).
    between(r, 2.9, 3.4, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from, {"-[MainMenuView clickADClose:]"},
                      "B closes the promo banner (its close is the screen's back button)");
    });
    press(r, 3.0, B);
    at(r, 3.45, [&r](mt_cpu *) {
        check(r.focus.active() && focus_text(r) == "focus on -[MainMenuView clickPlay:]",
              "the focus is on and on Play: " + focus_text(r));
    });
    // Help: down, A; B closes it.
    press(r, 3.5, DOWN);
    press(r, 3.8, A);
    at(r, 4.6, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[MainMenuView closeHelp:]",
              "Help is open, the focus on its close: " + focus_text(r));
    });
    press(r, 4.7, B);
    between(r, 3.45, 5.4, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from, {"-[MainMenuView clickHelp:]", "-[MainMenuView closeHelp:]"},
                      "down + A opens Help, B closes it");
    });
    // About: down (the focus is back on Help), A; down to the first link, A; down again
    // scrolls the text (no control below), which brings the other three links; A on each; B
    // closes About.
    press(r, 5.5, DOWN);
    press(r, 5.8, A);
    at(r, 6.5, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[MainMenuView closeAbout:]",
              "About is open, the focus on its close: " + focus_text(r));
    });
    press(r, 6.6, DOWN);
    press(r, 6.9, A);
    press(r, 7.2, DOWN); // nothing below: the text scrolls
    at(r, 8.9, [&r](mt_cpu *) {
        check(r.focus.stats().scrolls == 1 &&
                  focus_text(r) == "focus on -[MainMenuView clickLinkURL2:]",
              "down past the first link drags About's text; the focus goes to the 2nd link: " +
                  focus_text(r));
    });
    press(r, 9.0, A);
    press(r, 9.3, DOWN);
    press(r, 9.6, A);
    press(r, 9.9, DOWN);
    press(r, 10.2, A);
    press(r, 10.5, B);
    between(r, 5.45, 11.2, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from,
                      {"-[MainMenuView clickAbout:]", "-[MainMenuView clickLinkURL1:]",
                       "-[MainMenuView clickLinkURL2:]", "-[MainMenuView clickLinkURL3:]",
                       "-[MainMenuView clickLinkURL4:]", "-[MainMenuView closeAbout:]"},
                      "About: A on each of its four links (down scrolls the text to them), B "
                      "closes it");
        check(logged(r, from, "openURL:] http://maplestory.nexon.net: not opened"),
              "the links' openURL: are refused, as with taps (ADR 0049)");
    });
    // Game Center (right of About), More Games (right again), then back left and up to Play.
    press(r, 11.4, RIGHT);
    press(r, 11.7, A);
    press(r, 12.6, RIGHT);
    press(r, 12.9, A);
    between(r, 11.35, 13.6, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from,
                      {"-[MainMenuView clickGameCenter:]", "-[MainMenuView clickMoreGame:]"},
                      "right + A: Game Center, right + A: More Games");
        check(logged(r, from, "presentModalViewController:GKLeaderboardViewController"),
              "Game Center's leaderboard is presented (and dismissed, ADR 0034)");
    });
    press(r, 13.7, LEFT);
    at(r, 14.0, [&r](mt_cpu *) { // held up: repeats down the column to Play
        r.pad->button(UP, true);
    });
    at(r, 15.0, [&r](mt_cpu *) { r.pad->button(UP, false); });
    at(r, 15.1, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[MainMenuView clickPlay:]",
              "left, then up held (repeating), reaches Play: " + focus_text(r));
    });
    press(r, 15.3, A);
    between(r, 15.2, 17.0, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from, {"-[MainMenuView clickPlay:]"}, "A on Play starts the game");
        check(file_size(r, "theif0") == 1124 && file_size(r, "quest0") == 4781,
              "a fresh install's Play writes slot 0 (theif0 1124 bytes, quest0 4781)");
        check(!r.focus.active(), "the focus is off in the game (" + focus_text(r) + ")");
    });
    finish(r, 17.1);
}

// The Korean terms: A agrees (the focus starts on the agreement); then the two help pages:
// right pages the help's scroll view, B closes.
void terms(Run &r) {
    attach_pad(r);
    at(r, 2.9, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[TermsView releaseAgreementButton:]",
              "the terms show; the focus is on the agreement: " + focus_text(r));
    });
    press(r, 3.0, B); // nothing to go back to: the terms stay
    press(r, 3.3, A);
    between(r, 2.95, 4.0, [&r](mt_cpu *, std::size_t from) {
        check_actions(
            r, from, {"-[TermsView pressAgreementButton:]", "-[TermsView releaseAgreementButton:]"},
            "B does nothing on the terms; A agrees");
        check(file_size(r, "CTerms") == 2, "CTerms is written (2 bytes)");
    });
    press(r, 4.2, B); // the banner
    press(r, 4.5, DOWN);
    press(r, 4.8, A);
    press(r, 5.6, RIGHT);
    press(r, 7.0, B);
    between(r, 4.15, 7.7, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from,
                      {"-[MainMenuView clickADClose:]", "-[MainMenuView clickHelp:]",
                       "-[MainMenuView closeHelp:]"},
                      "B closes the banner, down + A opens Help, B closes it");
        check(logged(r, from, "drag ended, pages to page 1"),
              "right pages the Korean help to its 2nd page (a drag of its paging scroll view)");
    });
    finish(r, 7.8);
}

// A fresh install's Play by the pad: slot 0.
void seed(Run &r) {
    attach_pad(r);
    press(r, 3.0, B);
    press(r, 3.5, A);
    at(r, 5.0, [&r](mt_cpu *) {
        check(file_size(r, "theif0") == 1124, "the pad's Play on a fresh install writes slot 0");
    });
    finish(r, 5.1);
}

// A on Play at 0.05 s, while the engine is still in CStateTitle (#122): the focus drops it
// (#129), so no character select opens over the empty robinImages and the run goes on. Then the
// pad's Play works as on a fresh install (seed).
void early(Run &r) {
    at(r, 0.0, [&r](mt_cpu *) {
        r.pad = std::make_unique<virtual_pad::Pad>(virtual_pad::Kind::kXbox360);
        check(r.pad->ok(), "an Xbox 360 virtual pad is attached at the first turn " + r.pad->error);
    });
    at(r, 0.04, [&r](mt_cpu *cpu) {
        check(fe::focus_engine_at_title(cpu) &&
                  focus_text(r) == "focus on -[MainMenuView clickPlay:]",
              "at 0.04 s the engine is in CStateTitle, the focus on Play: " + focus_text(r));
    });
    between(r, 0.0, 0.6, [&r](mt_cpu *cpu, std::size_t from) {
        const fe::Focus::Stats &s = r.focus.stats();
        check(!fe::focus_engine_at_title(cpu) && s.early == 1 && s.taps == 0 &&
                  actions_since(r, from).empty(),
              "A at 0.05 s is dropped: no button action, the engine at its main menu by 0.6 s");
    });
    press(r, 0.05, A);
    press(r, 3.0, B);
    press(r, 3.5, A);
    at(r, 5.0, [&r](mt_cpu *) {
        check(r.focus.stats().taps == 1 && file_size(r, "theif0") == 1124,
              "A at 3.5 s is Play: a new game writes slot 0");
    });
    finish(r, 5.1);
}

// The character select (slot 0 saved): New, its alert's Yes: slot 1, the game loads.
void create(Run &r) {
    attach_pad(r);
    press(r, 3.0, A); // Play: the focus is on it (the banner is not shown again)
    at(r, 3.9, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[ChooseCharacterView chooseCharacterOne:]",
              "the character select; the focus on the first slot: " + focus_text(r));
    });
    // The buttons are a column right of the slots (Start, New, Delete): right past the slots
    // reaches Delete, the lowest; up is New. A; the alert's Yes is focused.
    press(r, 4.0, RIGHT);
    press(r, 4.3, RIGHT);
    press(r, 4.6, RIGHT);
    press(r, 4.9, UP);
    at(r, 5.15, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[ChooseCharacterView clickNew:]",
              "right, right, right, up: New: " + focus_text(r));
    });
    press(r, 5.2, A);
    // The alert is the engine's popup (CStateMainMenu opens it for New's '#': panel 7) under
    // the shell's alert view: its keys are the popup's (A yes, B no), as on every popup.
    at(r, 5.9, [&r](mt_cpu *cpu) {
        check(mt_input_panel() == 7 && popup_id(cpu) >= 0 && !r.focus.active(),
              "New asks: the engine's popup " + std::to_string(popup_id(cpu)) +
                  " on panel 7; the keys go to the game (" + focus_text(r) + ")");
    });
    press(r, 6.0, A);
    between(r, 2.9, 8.0, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from, {"-[MainMenuView clickPlay:]", "-[ChooseCharacterView clickNew:]"},
                      "A Play, the pad to New, A");
        check(file_size(r, "theif1") == 1124 && file_size(r, "quest1") == 4781,
              "A on the alert (OK, -5: Yes) writes slot 1, and the game loads");
    });
    finish(r, 8.1);
}

// Two slots saved: Delete with nothing chosen (the shell's own alert: B is its OK), then the
// first slot, Start.
void select(Run &r) {
    attach_pad(r);
    press(r, 3.0, A); // Play
    press(r, 4.0, RIGHT);
    press(r, 4.3, RIGHT);
    press(r, 4.6, RIGHT); // Delete
    press(r, 4.9, A);
    at(r, 5.6, [&r](mt_cpu *) {
        check(mt_input_panel() == -1 &&
                  focus_text(r) == "focus on -[ChooseCharacterView confirmAlert:]",
              "Delete with no character chosen: the shell's alert (no engine popup), the focus "
              "on its OK: " +
                  focus_text(r));
    });
    press(r, 5.7, B);
    at(r, 6.5, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[ChooseCharacterView clickDelete:]",
              "B closes it (its OK is the back button), the focus back on Delete: " +
                  focus_text(r));
    });
    press(r, 6.6, LEFT);
    press(r, 6.9, LEFT);
    press(r, 7.2, LEFT);
    press(r, 7.5, A); // the first slot
    at(r, 8.6, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[ChooseCharacterView clickStart:]" &&
                  r.focus.stats().follows == 1,
              "A on a saved slot chooses it and hands the focus to Start (ADR 0102): " +
                  focus_text(r));
    });
    press(r, 10.2, A);
    between(r, 2.9, 11.5, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from,
                      {"-[MainMenuView clickPlay:]", "-[ChooseCharacterView clickDelete:]",
                       "-[ChooseCharacterView confirmAlert:]",
                       "-[ChooseCharacterView chooseCharacterOne:]",
                       "-[ChooseCharacterView clickStart:]"},
                      "Play, Delete, its alert's OK, the first slot, Start");
        check(mt_input_panel() == 3 && !r.focus.active(),
              "Start loads the game: the key pad's panel 3 (" + focus_text(r) + ")");
        check(file_size(r, "theif0") == 1124 && file_size(r, "theif1") == 1124,
              "both slots are still saved");
    });
    finish(r, 11.6);
}

// Two slots saved: choose the second, Delete, its alert's Yes (the engine's popup: A), then
// B is Back to the main menu.
void delete_flow(Run &r) {
    attach_pad(r);
    press(r, 3.0, A); // Play
    press(r, 4.0, RIGHT);
    press(r, 4.3, A); // the second slot: the focus goes on to Start (ADR 0102)
    press(r, 5.4, DOWN);
    press(r, 5.7, DOWN); // Start, New, Delete
    at(r, 6.0, [&r](mt_cpu *) {
        check(focus_text(r) == "focus on -[ChooseCharacterView clickDelete:]",
              "right, A (the second slot; the focus on Start), down, down: Delete: " +
                  focus_text(r));
    });
    press(r, 6.1, A);
    // The shell's own alert (no engine popup): the focus starts on No, deleting takes a move.
    at(r, 6.8, [&r](mt_cpu *) {
        check(mt_input_panel() == -1 &&
                  focus_text(r) == "focus on -[ChooseCharacterView cancelAlert:]",
              "Delete asks; the focus on No: " + focus_text(r));
    });
    press(r, 6.9, LEFT);
    press(r, 7.2, A);
    at(r, 8.1, [&r](mt_cpu *) {
        check(file_size(r, "theif1") == 0 && file_size(r, "quest1") == 0 &&
                  file_size(r, "theif0") == 1124,
              "left, A (Yes) deletes the second character's files; the first stays");
        check(r.focus.active(), "the select screen again, the focus on: " + focus_text(r));
    });
    press(r, 8.2, B);
    between(r, 2.9, 9.0, [&r](mt_cpu *, std::size_t from) {
        check_actions(r, from,
                      {"-[MainMenuView clickPlay:]", "-[ChooseCharacterView chooseCharacterTwo:]",
                       "-[ChooseCharacterView clickDelete:]",
                       "-[ChooseCharacterView okDeleteAlert:]",
                       "-[ChooseCharacterView clickBack:]"},
                      "Play, the second slot, Delete, left + A: Yes, B: Back");
        check(focus_text(r) == "focus on -[MainMenuView clickPlay:]",
              "Back returns to the main menu, the focus on Play: " + focus_text(r));
    });
    finish(r, 9.1);
}

// A new game in the field by the pad only, then the screens of the field: an NPC's dialog, the
// popup menu (tabs, the item list, back, close), the cash shop's button and the map.
void game(Run &r) {
    attach_pad(r);
    // The main menu: B closes the banner, A is Play (no save: a new character, the intro).
    press(r, 3.0, B);
    press(r, 4.5, A);
    // The intro's speech boxes and MacArthur's first dialog: A (OK) each.
    for (const double t : {30.0, 34.0, 38.0, 42.0, 46.0, 51.0, 52.0, 53.0, 54.0}) {
        press(r, t, A);
    }
    at(r, 57.0, [](mt_cpu *cpu) {
        check(mt_input_panel() == 0 && game_mode(cpu) == 0,
              "A through the intro: the field (panel 0, game mode 0)");
    });

    // NPC chat (panel 5, key codes: no focus): a step right, up talks to MacArthur, A is OK
    // through his pages and closes the dialog.
    press(r, 60.0, RIGHT, 0.3);
    press(r, 61.0, UP, 0.6);
    at(r, 62.0, [&r](mt_cpu *cpu) {
        check(mt_input_panel() == 5 && game_mode(cpu) == 4 && !r.focus.active(),
              "right, up: MacArthur's dialog (panel 5, game mode 4; the focus off: " +
                  focus_text(r) + ")");
    });
    press(r, 62.5, A);
    press(r, 65.0, UP, 0.6);
    for (const double t : {67.0, 69.0, 70.5, 72.0}) {
        press(r, t, A);
    }
    at(r, 73.5, [](mt_cpu *cpu) {
        check(mt_input_panel() == 0 && game_mode(cpu) == 0,
              "A through every page: the dialog closed, the field again");
    });

    // The popup menu (panel 1, key codes; OK on a tab is a touch on it, ADR 0051).
    press(r, 76.0, START);
    at(r, 76.8, [](mt_cpu *cpu) {
        check(mt_input_panel() == 1 && game_mode(cpu) == 1 && menu_tab(cpu) == 0 &&
                  menu_sub(cpu) == 0,
              "Start opens the popup menu on its status tab");
    });
    press(r, 77.0, RIGHT);
    press(r, 77.6, A);
    at(r, 78.5, [&r](mt_cpu *cpu) {
        check(menu_tab(cpu) == 1 && menu_sub(cpu) == 1 && r.focus.stats().menu_tabs == 1,
              "right, A: the item tab opens, by a touch on it (its list's widgets enabled)");
    });
    press(r, 79.0, RIGHT); // the second item (Blue Potion)
    press(r, 79.6, A);     // chosen
    press(r, 80.4, A);     // used
    shot(r, 81.2, "items");
    press(r, 81.5, B);
    at(r, 82.3, [](mt_cpu *cpu) {
        check(game_mode(cpu) == 1 && menu_tab(cpu) == 1 && menu_sub(cpu) == 0,
              "B closes the tab: back to the tabs, the item tab still chosen");
    });
    press(r, 82.5, RIGHT);
    press(r, 83.0, RIGHT);
    press(r, 83.6, A);
    at(r, 84.4, [&r](mt_cpu *cpu) {
        check(menu_tab(cpu) == 3 && menu_sub(cpu) == 3 && r.focus.stats().menu_tabs == 2,
              "right, right, A: the skill tab opens, by a touch on it");
    });
    press(r, 85.0, B);
    at(r, 85.8, [](mt_cpu *cpu) {
        check(mt_input_panel() == 0 && game_mode(cpu) == 0,
              "B on the skill tab closes the popup menu (the game's: the item tab's B closes "
              "only the tab)");
    });

    // The cash shop's button (LT): during the tutorial the game answers with its popup 0x25B
    // ("This cannot be used right now", panel 7); A closes it.
    at(r, 89.0, [&r](mt_cpu *) { r.pad->trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1.0f); });
    at(r, 89.3, [&r](mt_cpu *) { r.pad->trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.0f); });
    at(r, 90.2, [](mt_cpu *cpu) {
        check(mt_input_panel() == 7 && popup_id(cpu) == 0x25B,
              "LT: the cash shop's answer in the tutorial, popup 0x25B (panel 7)");
    });
    press(r, 90.5, A);
    at(r, 91.5, [](mt_cpu *cpu) {
        check(mt_input_panel() == 0 && popup_id(cpu) == -1, "A closes it: the field");
    });

    // The map: Back opens it, B closes it (Back there switches to the world map).
    press(r, 92.0, SELECT);
    at(r, 93.0, [](mt_cpu *cpu) {
        check(mt_input_panel() == 8 && game_mode(cpu) == 3, "Back opens the map");
    });
    press(r, 93.5, B);
    at(r, 94.5,
       [](mt_cpu *cpu) { check(mt_input_panel() == 0 && game_mode(cpu) == 0, "B closes it"); });
    finish(r, 95.0);
}

// --- past the tutorial (#127) ------------------------------------------------------------------
//
// A post-tutorial save made by the pad alone, in two launches on one data directory (the
// fixture input.menu_shops_save, platform/menu_nav_test.cmake). The way is the tutorial's own
// (script/common/Tutorial*.out, docs/ANALYSIS.md): MacArthur's quests 0-6 on maps 2.8 and
// 2.9 (talk, jump, climb, a portal, a snail), the left portal to Henesys (2.0, chapter 1) and
// elder Stan; then Stan's "Elder's Advice" (main quest 7, five Red Snails on 2.3), whose end
// opens the first side quests' offers. Each step checks the quest it completes.

// Launch 1: a new game, the tutorial, Stan, saved on Stan's ledge in Henesys.
void tutorial(Run &r) {
    attach_pad(r);
    then_new_game(r);
    then_tutorial(r);
    then_save(r);
    then_do(r, "the save", [&r](mt_cpu *) {
        const std::vector<std::uint8_t> t = read_save(r, "theif0"), q = read_save(r, "quest0");
        check(t.size() == 1124 && t[0x41C] == 2 && t[0x41D] == 0 && q.size() == 4781 && q[0] == 1,
              "theif0 is saved in Henesys (area 2, map 0) and quest0 in chapter 1");
    });
    then_finish(r);
}

// Launch 2 (slot 0 saved by `tutorial`): Elder's Advice, five Red Snails on the Western Hill
// (2.3), back to Stan, saved.
void hunt(Run &r) {
    attach_pad(r);
    then_load_slot0(r);
    then_check(r, "the save loads in Henesys, quest 7 active",
               [](mt_cpu *cpu) { return hero(cpu).on(2, 0) && main_quest(7, 1)(cpu); });
    then_walk_to(r, 42); // down the steps, to portal200 (x 40)
    then_portal(r, 2, 3);
    then_fight(r, "five Red Snails (quest 7)", 2,
               [](mt_cpu *cpu) { return defeat_progress(cpu, hero(cpu), 0, 7) >= 5; }, 600,
               {2, 0, 42, 2, 3});
    then_check(r, "five Red Snails: quest 7's requirement 5/5",
               [](mt_cpu *cpu) { return defeat_progress(cpu, hero(cpu), 0, 7) >= 5; });
    then_walk_to(r, 900); // portal231 (x 904): Henesys, at portal200
    then_portal(r, 2, 0);
    then_walk_to(r, 502, 3);
    then_climb(r, 175);
    then_walk_to(r, 548);
    then_talk(r, "Stan: Elder's Advice (main quest 7) done", main_quest(7, 2));
    then_save(r);
    then_do(r, "the save", [&r](mt_cpu *) {
        const std::vector<std::uint8_t> q = read_save(r, "quest0");
        check(q.size() == 4781 && q[0] == 1 && q[1 + 7] == 2,
              "quest0 is saved with main quest 7 done");
    });
    then_finish(r);
}

// --- the shops and a quest offer (#127) -------------------------------------------------------

// Whether the screen has a control whose action is `action` (focused or not).
bool shows(const Run &r, const char *action) {
    for (const fe::FocusTarget &f : r.focus.targets()) {
        if (f.action == action) {
            return true;
        }
    }
    return false;
}
// The shop (game mode 2) on the key pad's panel 6, no popup.
bool in_shop(mt_cpu *cpu) {
    return mt_input_panel() == 6 && game_mode(cpu) == 2 && popup_id(cpu) == -1;
}
// A trigger pulled all the way for 0.15 s, then let go for `after` seconds.
void then_pull(Run &r, SDL_GamepadAxis a, double after = 0.5) {
    then(r, std::string("pull ") + SDL_GetGamepadStringForAxis(a), after + 1.2,
         [&r, a, after](mt_cpu *, double el) {
             r.pad->trigger(a, el < 0.15 ? 1.0f : 0.0f);
             return el >= 0.15 + after;
         });
}
// Presses `b` until `p` holds (each press 0.1 s, 0.4 s apart), at most `presses` times.
void then_press_until(Run &r, std::string what, SDL_GamepadButton b, Pred p, int presses) {
    then(r, std::move(what), presses * 0.5 + 1, [&r, b, p](mt_cpu *cpu, double el) {
        if (p(cpu) && std::fmod(el, 0.5) >= 0.1) {
            hold(r, b, false);
            return true;
        }
        hold(r, b, std::fmod(el, 0.5) < 0.1);
        return false;
    });
}

// What a step of the flow remembers for a later check.
struct Before {
    i32 meso = 0, count = 0;
    std::size_t log = 0;
};

// Buys the Buy tab's row `row` (item `id` at `price`): down to it, A (popup 3, "buy?"), A
// (popup 111, bought), A (back to the list); the meso and the item's count in guest memory.
void then_buy(Run &r, i32 row, i32 id, i32 price, i32 gets) {
    auto b = std::make_shared<Before>();
    then_press_until(
        r, "the Buy tab's row " + std::to_string(row), DOWN,
        [row](mt_cpu *cpu) { return shop(cpu).selected == row; }, row + 2);
    then_do(r, "before buying", [b, id](mt_cpu *cpu) {
        b->meso = hero(cpu).meso;
        b->count = item_count(cpu, hero(cpu), id);
    });
    then_check(r,
               "row " + std::to_string(row) + " sells item " + std::to_string(id) + " for " +
                   std::to_string(price) + " meso",
               [row, id, price](mt_cpu *cpu) {
                   const ShopRow w = shop_row(cpu, shop(cpu), row);
                   return w.id == id && w.price == price;
               });
    then_press(r, A);
    // Popup 5 asks how many (one, as it opens), popup 3 (a stack sold whole) whether to buy.
    then_wait_for(r, "how many / buy? (popup 5 or 3)", 5, [](mt_cpu *cpu) {
        return (popup_id(cpu) == 5 || popup_id(cpu) == 3) && mt_input_panel() == 7;
    });
    then_press(r, A);
    then_wait_for(r, "bought (popup 111)", 5, [](mt_cpu *cpu) { return popup_id(cpu) == 111; });
    then_do(r, "the purchase", [b, id, price, gets](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        check(h.meso == b->meso - price && item_count(cpu, h, id) == b->count + gets,
              "bought: meso " + std::to_string(b->meso) + " -> " + std::to_string(h.meso) +
                  ", item " + std::to_string(id) + " " + std::to_string(b->count) + " -> " +
                  std::to_string(item_count(cpu, h, id)));
    });
    then_press(r, A);
    then_wait_for(r, "the shop's list again", 5, in_shop, 0.3);
}

// The NPC the hero stands at (up) until its quest offer shows Accept and Refuse (A through the
// dialog before it).
void then_offer(Run &r, std::string npc) {
    auto chat = std::make_shared<double>(-1);
    then(r, "the offer of " + npc, 40, [&r, chat](mt_cpu *cpu, double el) {
        if (shows(r, "releaseAcceptButton:") && shows(r, "releaseRefuseButton:")) {
            release_all(r);
            return mt_input_panel() == 5;
        }
        if (mt_input_panel() == 5 || game_mode(cpu) == 4) {
            hold(r, UP, false);
            if (*chat < 0) {
                *chat = el;
            }
            hold(r, A, el - *chat >= 1.0 && std::fmod(el - *chat, 1.2) < 0.1);
            return false;
        }
        *chat = -1;
        hold(r, A, false);
        hold(r, UP, field(cpu) && std::fmod(el, 2.0) < 0.4);
        return false;
    });
}

// Down held `seconds` on the shop's list with menu_repeat `repeat` (#147, menu_input.h), then
// let go: the selection moves `rows` rows (one for the press; with the repeat one more at 0.40 s
// and every 0.15 s after), each repeat a release and a press of down on the game's key path.
// Then up, a row at a time, to the first row again; menu_repeat goes back off. The Sell tab:
// its rows are the hero's etc items (on this shop's Buy tab the selection stops at row 2).
void then_hold_down(Run &r, bool repeat, double seconds, i32 rows) {
    auto started = std::make_shared<bool>(false);
    auto from = std::make_shared<i32>(-1); // -1: no row selected yet (the first down selects 0)
    auto repeats = std::make_shared<std::uint64_t>(0);
    auto log = std::make_shared<std::size_t>(0);
    const std::string what = std::string("down held ") + std::to_string(seconds).substr(0, 4) +
                             " s on the Sell list, menu_repeat " + (repeat ? "on" : "off");
    then(r, what, seconds + 3,
         [&r, repeat, seconds, started, from, repeats, log](mt_cpu *cpu, double el) {
             if (!*started) {
                 *started = true;
                 r.menu.set_repeat(repeat);
                 *from = shop(cpu).selected;
                 *repeats = r.menu.stats().repeats;
                 *log = r.log.size();
             }
             hold(r, DOWN, el < seconds);
             return el >= seconds + 0.5;
         });
    then_do(r, what + ": the rows", [&r, what, rows, from, repeats, log](mt_cpu *cpu) {
        const i32 moved = shop(cpu).selected - *from;
        const std::uint64_t n = r.menu.stats().repeats - *repeats;
        std::size_t presses = 0;
        for (std::size_t i = *log; i < r.log.size(); i++) {
            presses += r.log[i].find("press down: Clet_HandleEvent(2, -2, 0)") != std::string::npos;
        }
        check(in_shop(cpu) && moved == rows && n == static_cast<std::uint64_t>(rows - 1) &&
                  presses == static_cast<std::size_t>(rows),
              what + ": the selection " + std::to_string(*from) + " -> " +
                  std::to_string(shop(cpu).selected) + " (" + std::to_string(rows) +
                  " rows wanted), " + std::to_string(n) + " repeats, " + std::to_string(presses) +
                  " key presses of down");
        r.menu.set_repeat(false);
    });
    then_press_until(
        r, "the first row again", UP, [](mt_cpu *cpu) { return shop(cpu).selected <= 0; }, 8);
}

// Launch 3 (slot 0 saved by `hunt`, Henesys): Mr. Wang's shop, the cash shop and the side
// quest of Humanoid (side quest 3, "8 Blue Snail Shells": level 7 and up).
void shops(Run &r) {
    attach_pad(r);
    then_load_slot0(r);
    then_check(r, "the save loads in Henesys with Elder's Advice (main quest 7) done",
               [](mt_cpu *cpu) { return hero(cpu).on(2, 0) && main_quest(7, 2)(cpu); });

    // Mr. Wang (Etc20, x 440 on Stan's ledge): his shop, panel 6, game mode 2.
    then_walk_to(r, 440);
    then_press_until(r, "Mr. Wang's shop", UP, in_shop, 6);
    then_check(r, "the shop: panel 6, game mode 2, the Buy tab, six rows (shop 3)",
               [](mt_cpu *cpu) {
                   const Shop s = shop(cpu);
                   return in_shop(cpu) && s.tab == 0 && s.kind == 1 && s.rows == 6;
               });
    then_check(r, "the Buy tab's Buy button shows", [&r](mt_cpu *) {
        return shows(r, "releaseBuyButton:") && shows(r, "releaseSellTabButton:");
    });
    // RB: the Sell tab ('6'); LB back to Buy ('4').
    then_press(r, RB);
    then_check(r, "RB: the Sell tab, its Sell button", [&r](mt_cpu *cpu) {
        return in_shop(cpu) && shop(cpu).tab == 1 && shows(r, "releaseSellButton:") &&
               shows(r, "releaseBuyTabButton:");
    });
    then_press(r, LB);
    then_check(r, "LB: the Buy tab again", [&r](mt_cpu *cpu) {
        return in_shop(cpu) && shop(cpu).tab == 0 && shows(r, "releaseBuyButton:");
    });
    // One Red Potion (item 3, 15 meso).
    then_buy(r, 0, 3, 15, 1);
    // Charge ('#', RT in menus): the Sell tab's throwing stars, refilled when the stack is
    // short. Bought just now the stack is full (200 of 200), so the shop opens nothing.
    then_buy(r, 2, 11, 170, 200);
    then_press(r, RB);
    // Down held on the Sell list (#147): one row without the repeat (the original), three with
    // it (the press, then 0.40 and 0.55 s). The list: the five etc items the save has.
    then_check(r, "the Sell tab lists at least five etc items", [](mt_cpu *cpu) {
        i32 n = 0;
        for (i32 i = 0; i < 48; i++) {
            n += etc_item(cpu, hero(cpu), i).id >= 0 ? 1 : 0;
        }
        std::printf("the Sell tab: %d etc items\n", n);
        return in_shop(cpu) && shop(cpu).tab == 1 && n >= 5;
    });
    then_hold_down(r, false, 1.0, 1);
    then_hold_down(r, true, 0.6, 3);
    auto stars = std::make_shared<i32>(-1);
    then_do(r, "the stars' slot", [stars](mt_cpu *cpu) {
        for (i32 i = 0; i < 48; i++) {
            if (etc_item(cpu, hero(cpu), i).id == 11) {
                *stars = i;
            }
        }
        check(*stars >= 0, "the Subi Throwing-Stars are in slot " + std::to_string(*stars));
    });
    then_press_until(
        r, "the stars' row on the Sell tab", DOWN,
        [stars](mt_cpu *cpu) { return shop(cpu).selected == *stars; }, 12);
    auto b = std::make_shared<Before>();
    then_do(r, "before Charge", [&r, b](mt_cpu *cpu) {
        b->meso = hero(cpu).meso;
        b->log = r.log.size();
    });
    then_pull(r, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    then_do(r, "RT: Charge", [&r, b, stars](mt_cpu *cpu) {
        const EtcItem e = etc_item(cpu, hero(cpu), *stars);
        check(logged(r, b->log, "press slot4: Clet_HandleEvent(2, 35, 0)"),
              "RT sends '#' (35) to the shop as a key event");
        check(in_shop(cpu) && hero(cpu).meso == b->meso && e.count == e.max && e.type == 2,
              "Charge on a full stack of stars (" + std::to_string(e.count) + " of " +
                  std::to_string(e.max) + "): no popup, no meso spent (" + where(cpu) + ")");
    });
    then_press(r, B);
    then_wait_field(r);
    then_check(r, "B closes the shop: the field", field);

    // The cash shop: LT (-26), panel 9, game mode 11; the focus over its tiles, no purchase.
    then_do(r, "before the cash shop", [&r, b](mt_cpu *cpu) {
        b->meso = hero(cpu).meso;
        b->log = r.log.size();
    });
    then_pull(r, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    then_wait_for(
        r, "the cash shop", 5,
        [&r](mt_cpu *cpu) {
            return mt_input_panel() == 9 && game_mode(cpu) == 11 && r.focus.active();
        },
        0.3);
    then_check(r, "the cash shop: the focus on its first tile (3,000 meso)",
               [&r](mt_cpu *) { return focused(r, "releaseMeso3000Button:"); });
    then_press(r, RIGHT);
    then_check(r, "right: the 10,000 meso tile",
               [&r](mt_cpu *) { return focused(r, "releaseMeso10000Button:"); });
    then_press(r, DOWN);
    then_check(r, "down: the EXP 80 tile under it",
               [&r](mt_cpu *) { return focused(r, "releaseExp80Button:"); });
    then_press(r, RIGHT);
    then_check(r, "right: the 120,000 meso tile",
               [&r](mt_cpu *) { return focused(r, "releaseMeso120000Button:"); });
    then_press(r, B);
    then_wait_field(r);
    then_do(r, "the cash shop closed", [&r, b](mt_cpu *cpu) {
        check(field(cpu), "B closes the cash shop (its Clear): the field (" + where(cpu) + ")");
        check_actions(r, b->log, {"-[TouchKeyPadView releaseClearButton:]"},
                      "B touched the cash shop's close");
        bool bought = false;
        for (const std::string &a : actions_since(r, b->log)) {
            bought = bought || a.find("CashShopKeyPadView") != std::string::npos;
        }
        check(!bought && hero(cpu).meso == b->meso, "no tile was pressed, no meso changed");
    });

    // Humanoid (Humanoid0, x 376 on the ground): Refuse, then Accept.
    then_walk_to(r, 42); // down the steps
    then_walk_to(r, 376);
    then_offer(r, "Humanoid");
    then_check(r, "the offer: side quest 3 not started", [](mt_cpu *cpu) {
        return quest_state(cpu, hero(cpu), 1, 3) == 0 && !quest_active(cpu, hero(cpu), 1, 3);
    });
    then_press(r, B);
    then_wait_field(r);
    then_check(r, "B (Refuse): side quest 3 still not started, back in the field", [](mt_cpu *cpu) {
        return field(cpu) && quest_state(cpu, hero(cpu), 1, 3) == 0 &&
               !quest_active(cpu, hero(cpu), 1, 3);
    });
    then_offer(r, "Humanoid");
    then_press(r, A);
    then_wait_for(
        r, "the field", 20,
        [](mt_cpu *cpu) { return field(cpu) && quest_active(cpu, hero(cpu), 1, 3); }, 0.5);
    then_check(r, "A (Accept): side quest 3 active (state 1)", [](mt_cpu *cpu) {
        return quest_state(cpu, hero(cpu), 1, 3) == 1 && quest_active(cpu, hero(cpu), 1, 3);
    });
    then_finish(r);
}

// --- the shop's list by the d-pad (ADR 0102) ---------------------------------------------------

// The shop's list as drawn: the cursor, the rows, the drag (MarketKeyPadView) and the first row
// the last paint showed (the tab's key scroll +4, which Shop_Paint writes from the drag).
struct ShopView {
    i32 tab = -1, cursor = -1, rows = 0, drag = 0, first = -1;
};
ShopView shop_view(mt_cpu *cpu) {
    ShopView v;
    const Shop s = shop(cpu);
    if (s.addr == 0) {
        return v;
    }
    const u32 pad = mt_input_pad_view();
    const u32 market = pad != 0 ? rd(cpu, pad + 60) : 0; // TouchKeyPadView.marketKeyPadView
    const u32 canvas = rd(cpu, k_canvas);
    const u32 game = canvas != 0 ? rd(cpu, canvas + 0x0C) : 0; // CStateGame
    v.tab = s.tab;
    v.cursor = s.selected;
    v.rows = fe::shop_list_rows(cpu, s.addr, game);
    v.drag = fe::shop_list_drag(cpu, market);
    const u32 scroll = s.tab == 0 || s.tab == 1
                           ? rd(cpu, rd(cpu, s.addr + 0x114) + 4 * static_cast<u32>(s.tab))
                           : 0;
    v.first = scroll != 0 ? rdi(cpu, scroll + 4) : -1;
    return v;
}
std::string shop_text(const ShopView &v) {
    return "tab " + std::to_string(v.tab) + " cursor " + std::to_string(v.cursor) + " of " +
           std::to_string(v.rows) + " rows, drag " + std::to_string(v.drag) + ", first row drawn " +
           std::to_string(v.first);
}
// The cursor's row lies wholly in the five rows drawn (38 points each), and the paint drew the
// list where the drag says.
bool cursor_in_view(const ShopView &v) {
    const i32 y = v.cursor * fe::kShopRowHeight + v.drag;
    return v.cursor >= 0 && v.cursor < v.rows && y >= 0 &&
           y + fe::kShopRowHeight <= fe::kShopRowsShown * fe::kShopRowHeight &&
           v.first == -v.drag / fe::kShopRowHeight;
}
// The popup's amount (popup 5 "how many": the popup object *(*0xF144C) +0x18).
i32 popup_amount(mt_cpu *cpu) { return rdi(cpu, rd(cpu, rd(cpu, 0xF144C)) + 0x18); }

// One press of `b` on the list, then: the cursor at `want` and wholly in view.
void then_list_step(Run &r, SDL_GamepadButton b, i32 want, const std::string &what) {
    then_press(r, b, 0.1, 0.3);
    then_do(r, what, [want, what](mt_cpu *cpu) {
        const ShopView v = shop_view(cpu);
        check(in_shop(cpu) && v.cursor == want && cursor_in_view(v),
              what + ": the cursor on row " + std::to_string(want) + ", in view (" + shop_text(v) +
                  ")");
    });
}

// LT held, a press of `b` (up or down) while it is, LT let go (ADR 0114): a page, or a plain
// direction where no list pages.
void then_page(Run &r, SDL_GamepadButton b) {
    then(r, std::string("LT + ") + SDL_GetGamepadStringForButton(b), 3,
         [&r, b](mt_cpu *, double el) {
             r.pad->trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, el < 0.35 ? 1.0f : 0.0f);
             hold(r, b, el >= 0.1 && el < 0.25);
             return el >= 0.7;
         });
}
// A page of the shop's list by LT + `b`: the cursor and the drag as shop_list_page has them,
// the cursor's row wholly in view, and no direction sent to the game.
void then_shop_page(Run &r, SDL_GamepadButton b, const std::string &what) {
    auto before = std::make_shared<ShopView>();
    auto log = std::make_shared<std::size_t>(0);
    then_do(r, what + ": before", [&r, before, log](mt_cpu *cpu) {
        *before = shop_view(cpu);
        *log = r.log.size();
    });
    then_page(r, b);
    then_do(r, what, [&r, b, before, log, what](mt_cpu *cpu) {
        const ShopView v = shop_view(cpu);
        const fe::ShopListView want = fe::shop_list_page(
            before->rows, fe::ShopListView{before->cursor, before->drag}, b == UP ? -1 : 1);
        check(in_shop(cpu) && v.cursor == want.cursor && v.drag == want.drag && cursor_in_view(v),
              what + ": the cursor on row " + std::to_string(want.cursor) + ", drag " +
                  std::to_string(want.drag) + ", in view (" + shop_text(v) + ")");
        check(!logged(r, *log, "press down: Clet_HandleEvent") &&
                  !logged(r, *log, "press up: Clet_HandleEvent"),
              what + ": no up or down reached the game");
    });
}

// A on the list's chosen row and A through what follows: how many or buy? (popup 5 or 3), bought
// (popup 111), the list again.
void then_buy_here(Run &r) {
    then_press(r, A);
    then_wait_for(r, "how many / buy? (popup 5 or 3)", 5, [](mt_cpu *cpu) {
        return (popup_id(cpu) == 5 || popup_id(cpu) == 3) && mt_input_panel() == 7;
    });
    then_press(r, A);
    then_wait_for(r, "bought (popup 111)", 5, [](mt_cpu *cpu) { return popup_id(cpu) == 111; });
    then_press(r, A);
    then_wait_for(r, "the shop's list again", 5, in_shop, 0.3);
}

// Launch 3 on the shops' save: Mr. Wang's Buy tab (six rows) and Sell tab by the d-pad. First
// without the hook, the original's key path: down stops on the third row (the cursor stays at 2
// while the key scroll moves and the paint puts it back). Then with it: down to the sixth row,
// the list scrolled so it shows; the amount set by the d-pad and two bought; up to the first
// row again; the Sell tab from its top down to its last row.
void shop_scroll(Run &r) {
    attach_pad(r);
    then_load_slot0(r);
    then_walk_to(r, 440);
    then_press_until(r, "Mr. Wang's shop", UP, in_shop, 6);
    then_check(r, "the Buy tab: six rows, nothing chosen", [](mt_cpu *cpu) {
        const ShopView v = shop_view(cpu);
        std::printf("the shop: %s\n", shop_text(v).c_str());
        return in_shop(cpu) && v.tab == 0 && v.rows == 6 && v.cursor == -1 && v.drag == 0;
    });
    then_do(r, "the hook off", [](mt_cpu *) { fe::install_shop_list_keys(false); });
    for (int i = 0; i < 5; i++) {
        then_press(r, DOWN, 0.1, 0.3);
    }
    then_do(r, "down x5 without the hook", [](mt_cpu *cpu) {
        const ShopView v = shop_view(cpu);
        check(in_shop(cpu) && v.cursor == 2 && v.drag == 0 && v.first == 0,
              "the original: five downs leave the cursor on row 2, the list unmoved (" +
                  shop_text(v) + ")");
    });
    for (int i = 0; i < 3; i++) {
        then_press(r, UP, 0.1, 0.3);
    }
    then_do(r, "the hook on", [](mt_cpu *cpu) {
        const ShopView v = shop_view(cpu);
        check(v.cursor == 0, "up x3: row 0 again (" + shop_text(v) + ")");
        fe::install_shop_list_keys(true);
    });
    for (i32 row = 1; row < 6; row++) {
        then_list_step(r, DOWN, row, "down");
    }
    then_check(r, "the sixth row scrolled the list by one row", [](mt_cpu *cpu) {
        const ShopView v = shop_view(cpu);
        return v.drag == -fe::kShopRowHeight && v.first == 1;
    });
    then_do(r, "a shot", [&r](mt_cpu *cpu) { write_shot(r, cpu, "buy_row5"); });
    then_list_step(r, DOWN, 5, "down on the last row");
    auto b = std::make_shared<Before>();
    auto id = std::make_shared<i32>(-1), price = std::make_shared<i32>(-1);
    then_do(r, "before buying the sixth row", [b, id, price](mt_cpu *cpu) {
        const ShopRow w = shop_row(cpu, shop(cpu), 5);
        *id = w.id;
        *price = w.price;
        b->meso = hero(cpu).meso;
        b->count = item_count(cpu, hero(cpu), w.id);
        std::printf("row 5: item %d for %d meso; the hero has %d and %d meso\n", w.id, w.price,
                    b->count, b->meso);
        check(w.id >= 0 && w.price > 0, "the sixth row sells an item");
    });
    then_press(r, A);
    then_wait_for(r, "how many (popup 5)", 5,
                  [](mt_cpu *cpu) { return popup_id(cpu) == 5 && mt_input_panel() == 7; });
    then_check(r, "the amount starts at 1", [](mt_cpu *cpu) { return popup_amount(cpu) == 1; });
    then_press(r, RIGHT);
    then_check(r, "right: 2", [](mt_cpu *cpu) { return popup_amount(cpu) == 2; });
    then_press(r, LEFT);
    then_check(r, "left: 1", [](mt_cpu *cpu) { return popup_amount(cpu) == 1; });
    // Up and down are ten more and ten fewer, within 1 and what the meso buys.
    auto most = std::make_shared<i32>(0);
    then_do(r, "the most the meso buys",
            [most, price](mt_cpu *cpu) { *most = std::min(hero(cpu).meso / *price, 100); });
    then_press(r, UP);
    then_do(r, "up: ten more", [most](mt_cpu *cpu) {
        check(popup_amount(cpu) == std::min(11, *most),
              "up: 11 or the most the meso buys, " + std::to_string(*most) + " (" +
                  std::to_string(popup_amount(cpu)) + ")");
    });
    then_press(r, DOWN);
    then_do(r, "down: ten fewer", [most](mt_cpu *cpu) {
        check(popup_amount(cpu) == std::max(1, std::min(11, *most) - 10),
              "down: ten fewer, at least 1 (" + std::to_string(popup_amount(cpu)) + ")");
    });
    // Right held 0.8 s with menu_repeat on (ADR 0114): the press and the repeats at 0.40, 0.55
    // and 0.70 s, each one more, up to the most the meso buys.
    then(r, "right held 0.8 s on the amount, menu_repeat on", 3, [&r](mt_cpu *, double el) {
        r.menu.set_repeat(true);
        hold(r, RIGHT, el < 0.8);
        return el >= 1.3;
    });
    then_do(r, "the amount repeated", [&r, most](mt_cpu *cpu) {
        r.menu.set_repeat(false);
        std::printf("right held 0.8 s: the amount %d (the most %d), %llu repeats\n",
                    popup_amount(cpu), *most,
                    static_cast<unsigned long long>(r.menu.stats().repeats));
        check(popup_id(cpu) == 5 && popup_amount(cpu) == std::min(5, *most),
              "right held: 1 + the press and three repeats = 5, or the most the meso buys (" +
                  std::to_string(popup_amount(cpu)) + ")");
    });
    then_press_until(r, "left to 2", LEFT, [](mt_cpu *cpu) { return popup_amount(cpu) == 2; }, 6);
    then_press(r, A);
    then_wait_for(r, "bought (popup 111)", 5, [](mt_cpu *cpu) { return popup_id(cpu) == 111; });
    then_do(r, "two bought", [b, id, price](mt_cpu *cpu) {
        const Hero h = hero(cpu);
        check(h.meso == b->meso - 2 * *price && item_count(cpu, h, *id) == b->count + 2,
              "the sixth row's item bought twice: meso " + std::to_string(b->meso) + " -> " +
                  std::to_string(h.meso) + ", count " + std::to_string(b->count) + " -> " +
                  std::to_string(item_count(cpu, h, *id)));
    });
    then_press(r, A);
    then_wait_for(r, "the shop's list again", 5, in_shop, 0.3);
    then_check(r, "the list as it was: row 5, scrolled by one", [](mt_cpu *cpu) {
        const ShopView v = shop_view(cpu);
        return v.cursor == 5 && cursor_in_view(v) && v.drag == -fe::kShopRowHeight;
    });
    for (i32 row = 4; row >= 0; row--) {
        then_list_step(r, UP, row, "up");
    }
    then_check(r, "row 0 is the top again", [](mt_cpu *cpu) { return shop_view(cpu).drag == 0; });
    // Two more etc items for the Sell tab: the stars (row 2) and the next row's.
    then_list_step(r, DOWN, 1, "down");
    then_list_step(r, DOWN, 2, "down");
    then_buy_here(r);
    then_list_step(r, DOWN, 3, "down");
    then_buy_here(r);
    // The Sell tab: the hero's etc items, its own cursor, the list from its top.
    then_press(r, RB);
    then_check(r, "RB: the Sell tab, from its top, more rows than it shows", [](mt_cpu *cpu) {
        const ShopView v = shop_view(cpu);
        std::printf("the Sell tab: %s\n", shop_text(v).c_str());
        return in_shop(cpu) && v.tab == 1 && v.drag == 0 && v.rows > fe::kShopRowsShown;
    });
    auto rows = std::make_shared<i32>(0);
    then_do(r, "the Sell tab's rows", [rows](mt_cpu *cpu) { *rows = shop_view(cpu).rows; });
    for (i32 row = 0; row < 12; row++) {
        then(r, "down on the Sell tab", 2, [&r, rows, row](mt_cpu *cpu, double el) {
            if (row >= *rows) {
                return true;
            }
            hold(r, DOWN, el < 0.1);
            if (el < 0.4) {
                return false;
            }
            const ShopView v = shop_view(cpu);
            check(v.cursor == row && cursor_in_view(v),
                  "down: Sell row " + std::to_string(row) + " in view (" + shop_text(v) + ")");
            return true;
        });
    }
    then_check(r, "the Sell tab's last row, the list scrolled to it", [rows](mt_cpu *cpu) {
        const ShopView v = shop_view(cpu);
        std::printf("the Sell tab's end: %s\n", shop_text(v).c_str());
        return *rows <= 12 && v.cursor == *rows - 1 && cursor_in_view(v) &&
               v.drag == fe::kShopRowHeight * (fe::kShopRowsShown - *rows);
    });
    then_do(r, "a shot", [&r](mt_cpu *cpu) { write_shot(r, cpu, "sell_last"); });
    // Paged with LT (ADR 0114): up a view from the last row, again to the top, down a view.
    auto pages = std::make_shared<fe::PageStats>();
    then_do(r, "the pages before", [pages](mt_cpu *) { *pages = fe::page_stats(); });
    then_shop_page(r, UP, "LT + up on the Sell tab's last row");
    then_shop_page(r, UP, "LT + up again");
    then_shop_page(r, DOWN, "LT + down from the top");
    then_shop_page(r, DOWN, "LT + down again");
    then_do(r, "the pages", [pages](mt_cpu *) {
        const fe::PageStats s = fe::page_stats();
        std::printf("pages: %llu, %llu moved\n",
                    static_cast<unsigned long long>(s.pages - pages->pages),
                    static_cast<unsigned long long>(s.moved - pages->moved));
        check(s.pages - pages->pages == 4 && s.moved - pages->moved == 4,
              "four pages, each moved the cursor");
    });
    then_do(r, "a shot", [&r](mt_cpu *cpu) { write_shot(r, cpu, "sell_paged"); });
    // LB and RB still switch the tabs.
    then_press(r, LB);
    then_check(r, "LB: the Buy tab",
               [](mt_cpu *cpu) { return in_shop(cpu) && shop(cpu).tab == 0; });
    then_press(r, B);
    then_wait_field(r);
    then_check(r, "B closes the shop: the field", field);
    then_do(r, "the hook's numbers", [](mt_cpu *) {
        const fe::ShopListStats s = fe::shop_list_stats();
        std::printf("shop list: %llu keys, %llu scrolls, %llu tab changes\n",
                    static_cast<unsigned long long>(s.keys),
                    static_cast<unsigned long long>(s.scrolls),
                    static_cast<unsigned long long>(s.tabs));
        check(s.scrolls >= 3 && s.tabs >= 1, "the hook scrolled the lists and reset a tab's");
    });
    then_finish(r);
}

// --- the popup menu's item and equipment grids by the d-pad (ADR 0102) -----------------------

// The item (tab 1) or equipment (2) grid: the cursor (CDataMgr +0xD8 / +0xDC plus 6 x the key
// scroll's first row, as the handlers use it) and the cells in use.
struct Grid {
    i32 cursor = -1, cells = 0, first = 0;
};
Grid grid(mt_cpu *cpu, i32 tab) {
    Grid g;
    const u32 mgr = data_mgr(cpu);
    const u32 scroll = rd(cpu, mgr + 0x10C);
    g.first = scroll != 0 ? rdi(cpu, scroll + 4) : 0;
    g.cursor = rdi(cpu, mgr + (tab == 1 ? 0xD8 : 0xDC)) + 6 * g.first;
    const u32 widget = rd(cpu, rd(cpu, tab == 1 ? 0xF14B8 : 0xF14C0));
    g.cells = widget != 0 ? static_cast<i32>(rd(cpu, widget + 8)) : 0;
    return g;
}
std::string grid_text(const Grid &g) {
    return "cursor " + std::to_string(g.cursor) + " of " + std::to_string(g.cells) +
           " cells (key scroll " + std::to_string(g.first) + ")";
}
// The equipment the save gets (equip_save).
constexpr i32 kEquipItems = 30;
// Grid `tab`'s widget, its drag, and whether the cursor's cell is wholly inside its rect.
u32 grid_widget(mt_cpu *cpu, i32 tab) { return rd(cpu, rd(cpu, tab == 1 ? 0xF14B8 : 0xF14C0)); }
i32 grid_drag(mt_cpu *cpu, i32 tab) { return rdi(cpu, grid_widget(cpu, tab) + 0x6C); }
bool grid_cell_in_view(mt_cpu *cpu, i32 tab) {
    const u32 w = grid_widget(cpu, tab);
    const Grid g = grid(cpu, tab);
    if (w == 0 || g.cursor < 0 || g.cursor >= g.cells) {
        return false;
    }
    const u32 c = rd(cpu, rd(cpu, w) + 4 * static_cast<u32>(g.cursor));
    const bool alone = cpu->mem[c + 0x5C] != 0;
    const i32 y = rdi(cpu, c + 0x24) + (alone ? 0 : rdi(cpu, c + 0x0C)) + rdi(cpu, w + 0x6C);
    const i32 h = rdi(cpu, c + 0x2C);
    const i32 top = rdi(cpu, w + 0x20), bottom = top + rdi(cpu, w + 0x28);
    return y >= top && y + h <= bottom;
}
// A press of `b` on grid `tab`, then: the cursor at `want`.
void then_grid_step(Run &r, i32 tab, SDL_GamepadButton b, i32 want, const std::string &what) {
    then_press(r, b, 0.1, 0.3);
    then_do(r, what, [tab, want, what](mt_cpu *cpu) {
        const Grid g = grid(cpu, tab);
        check(game_mode(cpu) == 1 && menu_sub(cpu) == tab && g.cursor == want,
              what + ": cell " + std::to_string(want) + " (" + grid_text(g) + ")");
    });
}

// Launch 3 on the shops' save (four items: Red and Blue Potions, two more): the popup menu's
// item grid by the d-pad. Without the hooks, down from the first cell goes six cells on, to an
// empty one past the drawn row; with them the d-pad keeps to the four cells drawn, and up from
// the top row goes back to the tab bar. Then the equipment grid, which equip_save filled with
// 30 items (8 rows, more than the grid shows): the d-pad through the hook, and LT + down / up
// paging it by the rows in view to its last row and back, the cell always in view (ADR 0114).
void popup_grids(Run &r) {
    attach_pad(r);
    then_load_slot0(r);
    then_press(r, START);
    then_wait_for(r, "the popup menu", 5, [](mt_cpu *cpu) { return game_mode(cpu) == 1; }, 0.3);
    then_press(r, RIGHT);
    then_press(r, A);
    then_wait_for(
        r, "the item grid", 5,
        [](mt_cpu *cpu) { return menu_sub(cpu) == 1 && grid(cpu, 1).cells > 0; }, 0.3);
    then_check(r, "the item grid: one row of four, the cursor on the first", [](mt_cpu *cpu) {
        const Grid g = grid(cpu, 1);
        std::printf("the item grid: %s\n", grid_text(g).c_str());
        return g.cursor == 0 && g.cells == 4;
    });
    then_do(r, "the hooks off", [](mt_cpu *) { fe::install_grid_keys(false); });
    then_grid_step(r, 1, DOWN, 6, "the original's down");
    then_do(r, "the hooks on", [](mt_cpu *cpu) {
        fe::install_grid_keys(true);
        std::printf("after the original's down: %s\n", grid_text(grid(cpu, 1)).c_str());
    });
    then_grid_step(r, 1, LEFT, 3, "any key from that empty cell: the last cell in use");
    for (const i32 want : {2, 1, 0}) {
        then_grid_step(r, 1, LEFT, want, "left");
    }
    then_grid_step(r, 1, LEFT, 0, "left on the first cell");
    for (const i32 want : {1, 2, 3}) {
        then_grid_step(r, 1, RIGHT, want, "right");
    }
    then_grid_step(r, 1, RIGHT, 3, "right on the last cell");
    then_grid_step(r, 1, DOWN, 3, "down with no row below");
    then_press(r, UP);
    then_check(r, "up from the top row: the tab bar (the item tab chosen, none open)",
               [](mt_cpu *cpu) {
                   return game_mode(cpu) == 1 && menu_tab(cpu) == 1 && menu_sub(cpu) == 0;
               });
    // The equipment grid: the save's equipment (equip_save, before this launch: 30 items, rows
    // of four), through the hook (PopupMenu_EquipTabOnKey 0x519F9), then paged with LT.
    then_press(r, RIGHT);
    then_press(r, A);
    then_wait_for(
        r, "the equipment grid", 5,
        [](mt_cpu *cpu) { return menu_sub(cpu) == 2 && grid(cpu, 2).cells > 0; }, 0.3);
    then_check(r, "the equipment grid: the save's 30 items, the cursor on the first",
               [](mt_cpu *cpu) {
                   const Grid g = grid(cpu, 2);
                   std::printf("the equipment grid: %s\n", grid_text(g).c_str());
                   return g.cursor == 0 && g.cells == kEquipItems && grid_cell_in_view(cpu, 2);
               });
    then_grid_step(r, 2, RIGHT, 1, "right on the equipment grid");
    then_grid_step(r, 2, DOWN, 5, "down: the next row as drawn");
    then_grid_step(r, 2, LEFT, 4, "left");
    then_grid_step(r, 2, UP, 0, "up");
    then_do(r, "a shot", [&r](mt_cpu *cpu) { write_shot(r, cpu, "equip_grid"); });
    // Pages: down by the rows in view until the last row, then up to the first.
    auto at = std::make_shared<i32>(0);
    for (int i = 0; i < 4; i++) {
        then_do(r, "before a page down", [at](mt_cpu *cpu) { *at = grid(cpu, 2).cursor; });
        then_page(r, DOWN);
        then_do(r, "LT + down on the equipment grid", [at](mt_cpu *cpu) {
            const Grid g = grid(cpu, 2);
            const i32 last_row = (kEquipItems - 1) / 4 * 4;
            const bool moved_rows = g.cursor > *at + 4 && (g.cursor - *at) % 4 == 0;
            const bool at_end = g.cursor >= last_row;
            std::printf("page down: %d -> %s, drag %d\n", *at, grid_text(g).c_str(),
                        grid_drag(cpu, 2));
            check(game_mode(cpu) == 1 && menu_sub(cpu) == 2 && (moved_rows || at_end) &&
                      grid_cell_in_view(cpu, 2),
                  "a page down: more than one row on, the cell in view (" + grid_text(g) + ")");
        });
    }
    then_check(r, "the last row, the grid scrolled to it", [](mt_cpu *cpu) {
        return grid(cpu, 2).cursor >= (kEquipItems - 1) / 4 * 4 && grid_drag(cpu, 2) < 0;
    });
    then_do(r, "a shot", [&r](mt_cpu *cpu) { write_shot(r, cpu, "equip_paged"); });
    for (int i = 0; i < 4; i++) {
        then_page(r, UP);
        then_check(
            r, "LT + up on the equipment grid: the cell in view, the grid open", [](mt_cpu *cpu) {
                return game_mode(cpu) == 1 && menu_sub(cpu) == 2 && grid_cell_in_view(cpu, 2);
            });
    }
    then_check(r, "the first row again, the grid at its top, still open (a page never leaves it)",
               [](mt_cpu *cpu) {
                   return grid(cpu, 2).cursor < 4 && grid_drag(cpu, 2) == 0 && menu_sub(cpu) == 2;
               });
    then_press(r, B);
    then_press(r, B);
    then_wait_field(r);
    then_do(r, "the hooks' numbers", [](mt_cpu *) {
        const fe::GridStats s = fe::grid_stats();
        const fe::PageStats p = fe::page_stats();
        std::printf(
            "grids: %llu keys, %llu scrolls; pages %llu (%llu moved)\n",
            static_cast<unsigned long long>(s.keys), static_cast<unsigned long long>(s.scrolls),
            static_cast<unsigned long long>(p.pages), static_cast<unsigned long long>(p.moved));
        check(s.keys == 14, "the hooks took the d-pad on both grids (10 + 4 keys)");
        check(p.pages == 8 && p.moved >= 4 && s.scrolls >= 2,
              "eight pages on the equipment grid, which scrolled");
    });
    then_finish(r);
}

// --- the gamepad pointer (#185, ADR 0107) -------------------------------------------------------
//
// pad_pointer = both, a virtual pad only: the right stick's axis events steer the pointer onto
// a control (closed loop: each turn the stick points at the control's tap point, pushed less
// as it nears it) and R3 held past 0.3 s taps there. In Korean (MAPLETHIEF_LANGUAGE=ko), whose
// Help has two pages: the terms' agreement, the main menu's banner close, About and its close,
// Help; then Help's paging scroll view is swiped by a held R3 and the stick (a swipe, which
// only a touch makes), and Help closed by the pointer. No d-pad, no A, no B.

constexpr SDL_GamepadButton RS = SDL_GAMEPAD_BUTTON_RIGHT_STICK;

void right_stick(Run &r, float x, float y) {
    r.pad->axis(SDL_GAMEPAD_AXIS_RIGHTX, static_cast<Sint16>(std::clamp(x, -1.0f, 1.0f) * 32767));
    r.pad->axis(SDL_GAMEPAD_AXIS_RIGHTY, static_cast<Sint16>(std::clamp(y, -1.0f, 1.0f) * 32767));
}

// The tap point of the reachable control whose action is `action`, if there is one now.
bool control_point(const Run &r, const std::string &action, float *x, float *y) {
    for (const fe::FocusTarget &t : r.focus.targets()) {
        if (t.action == action) {
            *x = t.px;
            *y = t.py;
            return true;
        }
    }
    return false;
}
bool has_control(const Run &r, const std::string &action) {
    float x = 0, y = 0;
    return control_point(r, action, &x, &y);
}

// Steers the pointer to (x, y), or to the control `action`'s tap point, with the right stick.
void then_steer(Run &r, const std::string &action, float x = 0, float y = 0) {
    then(r, "steer the pointer to " + (action.empty() ? std::string("a point") : action), 15,
         [&r, action, x, y](mt_cpu *, double) {
             float tx = x, ty = y;
             if (!action.empty() && !control_point(r, action, &tx, &ty)) {
                 right_stick(r, 0, 0);
                 return false; // not on the screen (yet)
             }
             const float dx = tx - r.pointer.x(), dy = ty - r.pointer.y();
             const float d = std::sqrt(dx * dx + dy * dy);
             if (d < 2.0f && r.pointer.shown()) {
                 right_stick(r, 0, 0);
                 return true;
             }
             // Past the dead zone always; full beyond 60 points.
             const float push = std::clamp(d / 60.0f, 0.35f, 1.0f);
             right_stick(r, d > 0 ? dx / d * push : push, d > 0 ? dy / d * push : 0);
             return false;
         });
}

// R3 held `seconds` (past its 0.3 s hold: the pointer's touch), then let go.
void then_tap(Run &r, double seconds = 0.45) {
    auto taps = std::make_shared<std::uint64_t>(0);
    then_do(r, "count the pointer's taps",
            [&r, taps](mt_cpu *) { *taps = r.pointer.stats().taps; });
    then_press(r, RS, seconds, 0.5);
    then_check(r, "R3 held taps once at the pointer",
               [&r, taps](mt_cpu *) { return r.pointer.stats().taps == *taps + 1; });
}

void pointer(Run &r) {
    r.settings.pad_pointer = fe::PadPointerMode::kBoth;
    attach_pad(r);
    auto from = std::make_shared<std::size_t>(0);
    const auto mark = [&r, from](const std::string &what) {
        then_do(r, "mark: " + what, [&r, from](mt_cpu *) { *from = r.log.size(); });
    };
    const auto ran = [&r, from](std::vector<std::string> want, const std::string &what) {
        then_do(r, "check: " + what,
                [&r, from, want, what](mt_cpu *) { check_actions(r, *from, want, what); });
    };
    const auto shows = [&r](const std::string &what, const std::string &action) {
        then_wait_for(r, what, 20, [&r, action](mt_cpu *) { return has_control(r, action); }, 0.5);
    };
    shows("the terms", "releaseAgreementButton:");
    then_steer(r, "releaseAgreementButton:");
    then_tap(r);
    then_check(r, "the pointer agrees to the terms: CTerms is written",
               [&r](mt_cpu *) { return file_size(r, "CTerms") == 2; });
    shows("the main menu with its banner", "clickADClose:");
    mark("banner");
    then_steer(r, "clickADClose:");
    then_tap(r);
    ran({"-[MainMenuView clickADClose:]"}, "the pointer taps the banner's close");
    // About, then its close.
    mark("about");
    then_steer(r, "clickAbout:");
    then_check(r, "the pointer is shown, the focus not used",
               [&r](mt_cpu *) { return r.pointer.shown() && r.focus.stats().uses == 0; });
    then_tap(r);
    shows("About open", "closeAbout:");
    then_check(r, "a pointer tap drops the focus highlight",
               [&r](mt_cpu *) { return r.pointer.highlight_dropped(); });
    then_steer(r, "closeAbout:");
    then_tap(r);
    ran({"-[MainMenuView clickAbout:]", "-[MainMenuView closeAbout:]"},
        "the pointer taps About on the main menu, then About's close");
    // Help, a swipe of its pages, its close.
    shows("the main menu again", "clickHelp:");
    mark("help");
    then_steer(r, "clickHelp:");
    then_tap(r);
    shows("Help open", "closeHelp:");
    then_steer(r, "", 400, 170);
    // The swipe: R3 held until the touch is down, the stick pushed left to x 90, R3 let go.
    then(r, "swipe Help's page left with the pointer", 10, [&r](mt_cpu *, double el) {
        hold(r, RS, true);
        if (!r.pointer.pressing()) {
            if (el >= 1) {
                check(false, "R3 held puts the pointer's touch down");
            }
            return false;
        }
        if (r.pointer.x() > 90) {
            right_stick(r, -1, 0);
            return false;
        }
        right_stick(r, 0, 0);
        hold(r, RS, false);
        return true;
    });
    then_wait(r, 1.0);
    then_do(r, "check: the swipe", [&r, from](mt_cpu *) {
        check(logged(r, *from, "drag ended, pages to page 1"),
              "the pointer's drag pages Help to its 2nd page (the help pages' swipe)");
    });
    then_steer(r, "closeHelp:");
    then_tap(r);
    ran({"-[MainMenuView clickHelp:]", "-[MainMenuView closeHelp:]"},
        "the pointer opens Help and taps its close");
    then_check(r, "no d-pad, OK or back was used: the focus never moved",
               [&r](mt_cpu *) { return r.focus.stats().uses == 0; });
    then_finish(r);
}

bool run_mode(Run &r, int argc, char **argv) {
    if (r.mode == "explore") {
        return explore(r, argc, argv);
    }
    if (argc != 0) {
        return false;
    }
    if (r.mode == "main_menu") {
        main_menu(r);
    } else if (r.mode == "terms") {
        terms(r);
    } else if (r.mode == "seed") {
        seed(r);
    } else if (r.mode == "early") {
        early(r);
    } else if (r.mode == "create") {
        create(r);
    } else if (r.mode == "select") {
        select(r);
    } else if (r.mode == "delete") {
        delete_flow(r);
    } else if (r.mode == "game") {
        game(r);
    } else if (r.mode == "tutorial") {
        tutorial(r);
    } else if (r.mode == "hunt") {
        hunt(r);
    } else if (r.mode == "shops") {
        shops(r);
    } else if (r.mode == "shop_scroll") {
        shop_scroll(r);
    } else if (r.mode == "popup_grids") {
        popup_grids(r);
    } else if (r.mode == "pointer") {
        pointer(r);
    } else {
        return false;
    }
    return true;
}

// equip_save (ADR 0114, before popup_grids): the copied save's character (DATA_DIR/Documents/
// theif0, docs/formats/save.md) gets kEquipItems equipment records in its equipment inventory
// (0x015, 48 x 16 bytes): item_equip ids 0 to 29, unrolled, 7 upgrade slots, no bonuses. Made at
// test time from the fixture's save; nothing of it is committed.
int equip_save(const fs::path &data) {
    const fs::path file = data / "Documents" / "theif0";
    std::vector<std::uint8_t> t = read_file(file);
    if (t.size() != 1124) {
        std::printf("FAIL: %s is %zu bytes, not 1124\n", file.string().c_str(), t.size());
        return 1;
    }
    for (i32 i = 0; i < 48; i++) {
        std::uint8_t *rec = t.data() + 0x15 + 16 * i;
        std::memset(rec, 0, 16);
        rec[0] = i < kEquipItems ? static_cast<std::uint8_t>(i) : 0xFF; // id; 0xFF: empty
        rec[2] = i < kEquipItems ? 7 : 0;                               // slots left
    }
    std::FILE *f = std::fopen(file.string().c_str(), "wb");
    if (f == nullptr || std::fwrite(t.data(), 1, t.size(), f) != t.size()) {
        std::printf("FAIL: cannot write %s\n", file.string().c_str());
        if (f != nullptr) {
            std::fclose(f);
        }
        return 1;
    }
    std::fclose(f);
    std::printf("equip_save: %d equipment items in %s\nall ok\n", kEquipItems,
                file.string().c_str());
    return 0;
}

int usage() {
    std::printf(
        "usage: menu_nav_real_test main_menu|terms|early|seed|create|select|delete|"
        "game|tutorial|hunt|shops|shop_scroll|equip_save|popup_grids|pointer|explore DATA_DIR "
        "OUT_DIR "
        "[STEP...]\n");
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        return usage();
    }
    Run r;
    r.mode = argv[1];
    r.data = argv[2];
    r.out = argv[3];
    if (!fs::is_regular_file(MT_ARMV7_SLICE) || !fs::is_directory(MT_BUNDLE_DIR)) {
        std::printf("skipped: %s or %s absent (scripts/bootstrap)\n", MT_ARMV7_SLICE,
                    MT_BUNDLE_DIR);
        return 77;
    }
    fs::create_directories(r.data);
    fs::create_directories(r.out);
    if (r.mode == "equip_save") {
        return equip_save(r.data);
    }
    // The pads' events reach the test without a focused window.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        std::printf("FAIL: SDL_Init(SDL_INIT_GAMEPAD): %s\n", SDL_GetError());
        return 1;
    }
    g_run = &r;
    r.focus.attach(r.actions);
    if (!run_mode(r, argc - 4, argv + 4)) {
        SDL_Quit();
        return usage();
    }
    std::stable_sort(r.steps.begin(), r.steps.end(),
                     [](const Step &a, const Step &b) { return a.at < b.at; });
    const int status = boot(r, read_file(MT_ARMV7_SLICE));
    g_run = nullptr;
    SDL_Quit();
    if (status != 0) {
        return 1;
    }
    std::printf("%s\n", g_failures == 0 ? "all ok" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}

#else

int main() {
    std::printf("skipped: built without generated code (work/gen: python -m recomp all)\n");
    return 77;
}

#endif
