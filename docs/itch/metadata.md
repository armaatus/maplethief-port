# itch.io page fields

The short fields of the itch.io project form for `armaatus.itch.io/maplethief` ([ADR 0146](../adr/0146-distribution-on-itch-io.md)); the description is [page.md](page.md). Joris fills them in the web editor (itch.io has no API for project pages).

| Field | Value |
|---|---|
| Title | MapleThief |
| Project URL | `maplethief` (armaatus.itch.io/maplethief) |
| Short description or tagline | MapleStory Thief Edition (2010, iPhone) on Windows, macOS, Linux, Steam Deck and Switch homebrew. Bring your own copy. |
| Classification | Games |
| Kind of project | Downloadable |
| Release status | Released (before the first push: Draft visibility) |
| Pricing | $0 or donate, with a suggested amount |
| Genre | Platformer |
| Tags | `fan-port`, `2d`, `side-scroller`, `action`, `rpg`, `retro`, `open-source`, `controller-support`, `steam-deck`, `nintendo-switch`, `homebrew` |
| Platforms (per upload, set by the butler channel) | `windows-x64`: Windows; `macos-universal`: macOS; `linux-x86_64`: Linux; `switch`: no platform tick (itch.io has none for consoles), display name "Nintendo Switch (homebrew)" |
| AI generation disclosure | Joris's answer (the port's code and docs are written with an AI coding agent; the page's images are not) |
| Community | Comments on |
| Links | Source: https://github.com/armaatus/maplethief-port; Buy Me a Coffee: https://buymeacoffee.com/armaatus |
| Cover image (630 x 500) and banner | Our own art, like the port's icon: no Nexon logo, no game art |
| Screenshots | Gameplay only, captured from a local build, uploaded here only, never committed |
| Custom noun for downloads | (empty) |

Uploads: the butler channels name the platforms, so each upload gets its platform tick once after the first push (`butler status armaatus/maplethief` lists them). Pre-releases (`-rc.N`) are not pushed.
