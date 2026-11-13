# Sharing packs

These are the rules for sharing packs in the project's places: the **Packs** category of [GitHub Discussions](https://github.com/armaatus/maplethief-port/discussions), and any reply, issue or pull request that points at a pack. The category's pinned post links here. The reasons are in [ADR 0149](../adr/0149-pack-manifest-several-packs-and-what-the-project-hosts.md) and [What's included, and what isn't](../legal.md#packs).

## What you can share here

Only packs with **nothing from the game** in them:

- **Translations of the port's own menus**: a text pack with `Port.strings` only.
- **Your own audio**: music you composed and sounds you recorded or made yourself, as an audio pack.

## What you can't share or link here

- **Texture packs of any kind**: upscaled, redrawn, edited, or new art made to replace the game's pictures.
- **Translations of the game's text**: dialogue, names or descriptions (`Scripts.strings`, `ScriptExtra.strings`, `Tables.strings`). A pack that has these next to `Port.strings` counts as one.
- **Audio made from the game's**: remasters, re-encodes, remixes, covers or arrangements of its music, edits of its sounds.
- **Texture dumps, text exports, or any of the game's files.** These are the game itself; please don't share them anywhere.
- **Links** to any of the above, or to where they can be found. Also no IPAs, no links to them, and no help getting or decrypting one.

Packs like these may exist elsewhere. The project doesn't host them, link them or answer questions about where to find them.

## How to post a pack

1. Give the pack a `pack.strings` with at least `format`, `kind`, `id` and `name` ([ADR 0149](../adr/0149-pack-manifest-several-packs-and-what-the-project-hosts.md#the-manifest)), plus `content = "own";` and a `license`.
2. Check it with the game's pack check, once there is one (#371), and play with it.
3. Start a discussion in **Packs** titled with the pack's name and version. Say what it changes, the game version and port version it was made for, and the licence. Attach the pack as a zip, or link to where you host it.
4. By posting, you confirm that you made everything in it, or have the right to share it under the licence you name.

## What happens to a post that breaks the rules

A maintainer edits out the file or link, or removes the post, and replies with a link to this page. A repeat gets the author blocked from the repository. If you think a post here breaks the rules, use **Report content** on it.
