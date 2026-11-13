# 149. One manifest for every pack kind, several packs at once, and what the project hosts

- **Status:** Accepted (Joris, 2026-11-13): the hosting policy, [Joris on #369](https://github.com/armaatus/maplethief-port/issues/369). Accepted (delegated by Joris, 2026-11-13): the manifest, the load order and the compatibility rules (listed under [Delegated calls](#delegated-calls))
- **Date:** 2026-11-13
- **Issue:** #369 (epic #294)
- **Builds on:** [ADR 0081](0081-texture-packs-and-nearest-texture-filter.md) (texture packs), [ADR 0100](0100-public-repository-and-player-releases.md) (what the repository and the releases hold), [ADR 0106](0106-audio-packs.md) (audio packs), [ADR 0108](0108-text-packs.md) (text packs and their `.strings` syntax), [ADR 0127](0127-data-folder-next-to-the-game.md) (the data folder), [ADR 0142](0142-platform-capabilities-and-the-switch-frontend.md) (the pack folders)

## Context

Epic #294 gives packs a screen (#370), tools that need no checkout (#371) and shipped translations of the port's menus (#373). Each of these needs three things settled first.

- **Only text packs describe themselves.** A text pack has `textpack.strings` (`format`, `name`, `language`; optional `version`, `game_version`, `author`; ADR 0108). A texture pack (ADR 0081) and an audio pack (ADR 0106) are bare folders: a packs screen could show only a folder name.
- **One pack per kind.** `texture_pack`, `audio_pack` and `text_pack` each name one folder; `mt_texpack_open` closes the pack before it (`runtime/include/maplethief/texpack.h`), and the audio and text packs do the same. A player cannot add a sound-effects pack beside a music pack, or the port's menus in Dutch beside a translation of the dialogue.
- **No compatibility statement.** The port accepts one game, version 1.5, by its main binary (ADR 0135). A pack made for a later port with new keys, or a later manifest format, has no way to say so: it would load and do half of what it means.
- **The legal line.** [legal.md](../legal.md) says texture dumps and packs made from them contain the game's files and must not be shared. A redrawn or upscaled texture derives from the game's art, a translation from its text, a remaster or remix from its music. An audio pack of someone's own music, or a translation of the port's own menus, derives from nothing of the game's. The project needs one statement of what it hosts, links or leaves alone, before #330 opens a place where players share packs.
- **Packs are untrusted.** Every rule here keeps ADR 0081's: pack files are downloads, parsed within limits, never executed.

## Options considered

**The manifest's file and syntax.**

1. **`pack.strings` in every pack, in ADR 0108's `.strings` syntax; a text pack's `textpack.strings` still read when `pack.strings` is missing.** One parser (runtime and Python) already applies the limits, refuses bad UTF-8 and reports `line N: reason`. Existing text packs keep working. Chosen.
2. A JSON manifest (`pack.json`). A second parser for untrusted input, and text pack authors would write two syntaxes.
3. Per-kind names (`texturepack.strings`, `audiopack.strings`). The kind is a field; a name per kind only adds ways to get it wrong. `audiopack.txt` (ADR 0106) is already the list of sounds, not a manifest.

**One kind per pack, or mixed packs.**

1. **One kind per pack.** Each folder keeps the layout its runtime already reads, and the packs screen orders each kind separately. A "theme" with art and sound is two packs. Chosen.
2. Mixed packs with `textures/`, `audio/`, `text/` subfolders. Every existing pack would move, and a mixed pack's order would have to be the same for its three kinds.

**Which pack wins.**

1. **The list's order: the first pack that has a usable entry wins, entry by entry.** An entry is a texture file name, an audio sound id, or a text key in one table. A rejected entry falls through to the next pack, then to the game. This is what a player expects from a list with "move up": the top one shows. Chosen.
2. The last pack wins (a mod loader's "load order"). Equally workable, but the list on screen would read upside down.
3. Whole-pack precedence (the first pack hides every later pack of its kind). A music pack would then hide an effects pack.

**Text packs in different languages.** Allowing them would draw a German dialogue beside a French menu and could not decide the fallback font's language. Refusing the later pack with a reason keeps one language on screen. Chosen.

**Compatibility.**

1. **Three fields: `format` (the manifest's own version), `game_version` (the game it was made from) and `port_min` (the oldest port it works with).** A pack whose `format` or `port_min` is newer than the running port, or whose `game_version` is not one the port runs, is refused with that reason. Missing fields are accepted: packs made before this ADR still load. Chosen.
2. A port version range (`port_max` too). A pack cannot know what a later port will break; when the port breaks a pack format, it raises `format`.

## Decision

### The manifest

Every pack may have `pack.strings` at its top, in ADR 0108's syntax (strict UTF-8, `"key" = "value";`, the same limits, refused whole on a broken rule). Keys:

| Key | Required | Meaning |
|---|---|---|
| `format` | yes | `"1"`. A larger number is refused (`pack format 2 needs a newer port`). |
| `kind` | yes | `texture`, `audio` or `text`. A pack opened as another kind is refused. |
| `id` | yes | The pack's identity across versions: 1 to 64 of `[a-z0-9._-]`, starting with a letter or digit. Two packs with one `id` in one list: the second is skipped and logged. |
| `name` | yes | The name the packs screen shows, at most 64 characters. |
| `version` | no | The pack's own version, shown as is (at most 32 characters). |
| `author` | no | Shown as is (at most 128 characters). |
| `description` | no | One paragraph for the packs screen (at most 1,024 characters). |
| `language` | text: yes; else no | A BCP 47 tag, ADR 0108's field. |
| `game_version` | no | `1.5`, the game's version the pack was made from. Any other value is refused (`made for the game's version 1.6; this port runs 1.5`). |
| `port_min` | no | The oldest port version it works with (`1.4.0`). A newer one than the running port is refused (`needs port 1.4.0 or later; this is 1.3.2`). |
| `content` | no | `own` (nothing in it comes from the game) or `derived` (made from the game's art, sound or text). Shown on the packs screen; the runtime does not act on it. Pack sharing in the project's places requires `own` ([below](#what-the-project-hosts-links-and-leaves-alone)). |
| `license` | no | An SPDX identifier or `custom`; required for a pack shared in the project's places. |

Unknown keys are ignored, so a later `format` 1 manifest can add optional keys. A pack without `pack.strings` still loads: a texture or audio pack takes its folder's name as `name` and `id`; a text pack reads `textpack.strings` as before, with `kind` text and `id` its folder's name. When a folder has both, `pack.strings` counts and `textpack.strings` is ignored, logged once. `mtport.textpack export` keeps writing `textpack.strings` until #371 moves the tools to `pack.strings`.

### Several packs at once

- **The settings** keep their keys (`texture_pack`, `audio_pack`, `text_pack`) and take an ordered list of folders, separated by `|`, first first. A single path is a list of one, so every existing `settings.ini` means what it meant. A folder whose path contains `|` cannot be listed (Windows forbids the character in paths; the packs screen refuses it with that reason). The command-line options (`--texture-pack` and the others) may be given more than once, in order.
- **At most 16 packs of a kind.** The 17th and later are skipped and logged.
- **Entries, by kind.** The first pack in the list that has the entry, and whose file passes the kind's checks (ADR 0081, 0106, 0108), wins. A rejected file is logged once and the next pack is asked, then the game.
  - **Texture:** by file name (`<hash>_<W>x<H>.png`, `.raw.png`, `ui/<path>`). Packs of different k mix: each file is checked against its own upload. The runtime keeps one replacement hook at `MT_GFX_REPLACE_ORDER_PACK` that walks its packs, so the chain's 8 slots are not spent on packs.
  - **Audio:** by sound id (`31` whatever the extension). A `.loop` file belongs to the pack whose sound won: one pack's loop never applies to another pack's file.
  - **Text:** by key within its table (`Scripts`, `ScriptExtra`, `Tables`, `Port`). A text pack whose `language` differs from the first text pack's is refused (`language fr differs from the first text pack's de`).
- **Hidden entries are counted.** At the end, the exit log adds per pack what it replaced and what an earlier pack hid (`texpack: HDSprites 1.2: 812 replaced, 40 hidden by UIRedraw`); the packs screen (#370) shows the same counts and every refusal's reason.
- **Pack folders** (ADR 0142's `texture-packs/`, `audio-packs/`, `text-packs/` beside `game/`) are where the packs screen looks on every platform, not only the Switch. A folder anywhere else can still be listed by path.
- **A zip** dropped on the packs screen (#370) is unpacked into its pack folder under the same untrusted-input rules as the pack's files: no absolute paths or `..`, no links, at most the kind's file limits and 1 GiB in all, refused whole on the first breach.

### What the project hosts, links and leaves alone

Joris's decision: the project hosts only packs with no game content, its own translations of the port's menus and own-made audio. Packs made from the game's art or text are tolerated elsewhere; the project never hosts them and never links to them.

| Pack | Repository and releases | Project's Discussions: upload or link | Elsewhere |
|---|---|---|---|
| Translation of the port's own menus (`Port.strings` only, `content = own`) | Yes: shipped packs (#373) | Yes | Fine |
| Own-made audio: own compositions and recordings with the author's licence (`content = own`) | No: the project ships no audio pack (ADR 0106) | Yes | Fine |
| Remaster, re-encode, remix, cover or arrangement of the game's music or sounds | No | No | Tolerated, not linked |
| Translation of the game's text (`Scripts`, `ScriptExtra` or `Tables` keys) | No | No | Tolerated, not linked |
| Texture pack of any kind: upscaled, redrawn, edited, or new art for the game's textures | No | No | Tolerated, not linked |
| Texture dump, text export, a copy of any game file | No | No | Must not be shared: these are the game's files |

"Link" means any pointer the project controls: the repository, the docs, release notes, the itch.io page, the compendium site, Discussions posts and replies. A post that shares or links a pack from a "No" row is edited or removed, with a pointer to the rules. A text pack that mixes `Port.strings` with the game's keys is a translation of the game's text. "Tolerated" means the project does not chase such packs; it does not mean they are allowed by Nexon, and [legal.md](../legal.md) still asks players to keep packs made from the game to themselves.

The rules for the Discussions category that #330 opens are in [docs/play/sharing-packs.md](../play/sharing-packs.md); the category's pinned post links to that page rather than copying it, so the rules have one source.

### Delegated calls

Made by the agent under Joris's delegation of 2026-11-13; Joris can veto any of them on the PR.

1. The manifest is `pack.strings` in ADR 0108's syntax, `textpack.strings` still read.
2. One kind per pack.
3. The first pack in the list wins, entry by entry; at most 16 packs of a kind; `|` separates the list in the existing settings.
4. Text packs of different languages are not mixed.
5. Compatibility is `format`, `game_version` and `port_min`; no upper bound.
6. Texture packs are never hosted or linked, including art drawn from scratch: every texture pack replaces the game's own pictures by their hashes, so its pictures depict the game's characters, items and screens, and nobody can check the line between "new" and "redrawn".
7. Remixes, covers and arrangements of the game's music count as derived, like remasters: the compositions are Nexon's.
8. The rules live on a docs page, `docs/play/sharing-packs.md`, linked from the category.

## Consequences

- #370 can list every pack with a name, author and version, order packs per kind, and say why one was refused, from one manifest parser.
- #371's checks share the manifest rules with the runtime; the texture and audio checks gain one more file to read.
- #373's shipped translations are packs like any other (`content = own`, `license = MIT`) and are the only packs the repository holds.
- A player can stack a menu translation on a dialogue translation of the same language, or an effects pack under a music pack.
- The runtime changes (the list in each setting, one hook walking several texture packs, the hidden counts) are #370's work; until it lands, the first folder of a list is the only one opened.
- The `content` field is the author's statement, not a check. Moderation of the Discussions category relies on it plus a look at the files.
- How we'd know this is wrong: players stacking packs and reporting surprises about which one shows (the order), packs refused for a `port_min` that a later port could have run (the compatibility rule too strict), or a Nexon concern about a pack in a "Yes" row (the line drawn in the wrong place).
