# 190. The menus read aloud: the focus, the list cursors and a name table for the shell's buttons

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#393](https://github.com/armaatus/maplethief-port/issues/393) (epic [#398](https://github.com/armaatus/maplethief-port/issues/398))
- **Builds on:** [ADR 0174](0174-text-to-speech-layer-system-voices-and-queue.md) (the speech layer, `SpeechKind::kMenu` interrupts), [ADR 0187](0187-text-events-dialogue-and-scene-boxes-for-speech.md) (reading guest text without calling guest code), [ADR 0051](0051-controller-focus-navigation.md) (the focus over UIKit controls), [ADR 0102](0102-controller-lists-and-the-character-select-hand-off.md) (the list cursors), [ADR 0108](0108-text-packs.md) (port text)

## Context

With speech on (#390), a player who can't read the screen also needs to know where they are in the menus: the port's settings menu, the shell's UIKit screens (main menu, Help, About, the terms, the character select, their alerts, the cash shop's tiles), the popup menu's item, equipment and skill lists, and the shops. [docs/re/speech.md](../re/speech.md#the-menus) found what each shows:

- The settings menu's rows are port text (`tr()`), with a label and a value.
- The shell's buttons have no title. #378 assumed they are images keyed by file; the `input.menu_*` flows with the speech probe found that only two have a named image. The rest are transparent buttons over art the views draw in `-drawRect:`, and each has only its action.
- The popup menu's lists and the shop's rows hold item and skill ids. The names are in the data tables, which the game reads through `CDataMgr::FindRow` and `ReadString`, and those allocate on the guest heap.

## Options considered

1. **Say what the focus or cursor lands on, once per change** (chosen). It is the screen reader's model: a move says the new place, and nothing else is said. Each source has a reader the frontend polls once per turn, and one de-duplicating step compares it with what was said last.
2. **Say the whole screen when it opens** (every label and button). Long, and a move then says nothing new. Only the first label that appears is said, as the screen's title.
3. **Name the shell's buttons by OCR of their art, or by their position.** OCR is slow and unreliable on the game's art. Positions change with the screen and the language.
4. **Call the game's table lookups** to get names. They allocate guest memory, so the game would run differently with speech on (ADR 0187's rule).
5. **A new "describe" action for the description.** The game already has a detail view: OK on a row opens its sub-list (use, equip, level up). Saying the description when that opens needs no new binding.

## Decision

`platform/src/speech_menus.{h,cpp}`: `SpeechMenus` (`Game::speech_menus`) says `SpeechKind::kMenu` utterances, each **once per change** of what is said.

- **The settings menu**, after each event it takes: "tab: label, value" when it opens or the tab changes, "label, value" when the row changes, the value alone when only the value changes. A change of a speech row says nothing, so the sample in the new voice is not cut off.
- **The shell**, each turn while the focus is active: the focused control's name from `kShellButtons`, keyed by the control's image (`mt_uikit_control::image`, the `imageNamed:` name without folder, `.png` and `_kor`) when it has one, else by its action `-[Target selector:]`; else the text of the label it holds. The names are port text (`button_*` in `port_text.def`), so a text pack translates them. The first `UILabel` that appears (`mt_uikit_labels`, a new read of the view tree) is said before the control, as the screen's title.
- **The popup menu and the shop**, each turn (`read_menu_spot`): the item or skill under the list's cursor, by id, named from the data tables (`mt_text_table_field`, a host-side read of the row by its key, CP949 converted). Shop rows also say their price. When the game's sub-list opens on a row, its description is said.
- **Nothing the player can't see**: the skill tab's list before its touch (`popup_menu_list_hidden`), a quest's page, a closed menu, an inactive focus. With speech off nothing is read (no label walk, no table read), and what was said is forgotten. Turning speech on then says the current place.

The runtime gains three read-only calls: the button's image name in `mt_uikit_control`, `mt_uikit_labels`, and `mt_text_table_field`. None of them writes guest memory or calls guest code.

## Consequences

- `platform_test` checks the rules on synthetic data: each settings row and value once per move, a name for every control of a synthetic tree of the shell's screens, the rows once per move, the description once. With `MT_SPEECH_MENUS=1`, the real-asset `input.menu_*` flows and `input.speech_menus_skills` list every control and row they reach and fail on one without a name, or on a move that is not said exactly once. Their output hashes the text.
- A new shell button with no entry fails those flows by name. It is a table row to add, not a code change.
- The quest tab and the NPC chat's quest rows are not said yet; the popups' texts are #392's.
