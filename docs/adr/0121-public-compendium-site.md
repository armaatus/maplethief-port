# 121. The public compendium site

- **Status:** Accepted
- **Date:** 2026-11-06
- **Issue:** #211 (epic #210)
- **Revises:** [ADR 0100](0100-public-repository-and-player-releases.md) (for the compendium site only)

## Context

Epic #210 builds a public compendium of MapleStory Thief Edition (iOS, version 1.5): every monster, item, NPC, shop, trade, skill, map and quest, quest solutions, chapter walkthroughs, mechanics and the game's history. The port has decoded every file in the bundle ([formats/](../formats/README.md)) and mapped the binary ([re/](../re/)), so everything such a site needs is already parsed.

ADR 0100 says the repository and the player releases never carry the game's art, audio, maps, tables or text, and that builds bundling the assets stay personal and are never uploaded. A compendium publishes facts, sprites, icons, map renders and dialogue derived from those files. It therefore needs its own decision, and that decision revises ADR 0100 for the site only. The repository and the player releases keep ADR 0100's rules unchanged.

Constraints:

- CI never sees the IPA, and `CLAUDE.md` forbids committing converted assets, screenshots or crops of game art. The site can only be built on a machine with `work/ipa/`, from that builder's own copy.
- The site must reach GitHub Pages without derived files landing on `main`, and preferably without any commit of derived files at all.
- The repository is public. Pages is switched off (`has_pages: false`).

Comparable fan wikis for MapleStory and other games publish sprites, item data and dialogue, and rights holders generally tolerate them. Nexon has not ruled on this game. Like ADR 0100, this ADR is not legal advice; it records a risk the project accepts.

### Can the deploy workflow read a draft-release asset?

Option (a) below depends on this, so it was checked against GitHub's REST documentation (2026-11-06):

- [List releases](https://docs.github.com/en/rest/releases/releases#list-releases): "Information about published releases are available to everyone. Only users with push access will receive listings for draft releases."
- [Get a release by tag name](https://docs.github.com/en/rest/releases/releases#get-a-release-by-tag-name) returns only a published release, so a draft cannot be found by its tag.
- [Permissions required for GitHub Apps](https://docs.github.com/en/rest/authentication/permissions-required-for-github-apps), Contents: `GET /repos/{owner}/{repo}/releases` and `GET /repos/{owner}/{repo}/releases/assets/{asset_id}` need Contents read. A workflow's `GITHUB_TOKEN` is an installation token of the GitHub Actions app with the permissions of the workflow's `permissions:` block.
- [Get a release asset](https://docs.github.com/en/rest/releases/assets#get-a-release-asset): with `Accept: application/octet-stream` the API streams the asset or redirects to it (200 or 302).

So the endpoints need Contents read, but a draft is listed only to a caller with push access, which for `GITHUB_TOKEN` is `contents: write`. With `contents: read` the list comes back without drafts. A user report confirms this in practice: drafts were missing from `gh release list` in a workflow until the token had write access to that repository ([cli/cli#9076](https://github.com/cli/cli/issues/9076)). The deploy workflow therefore declares `contents: write`, finds the draft by listing the releases, and downloads the asset by its id. A token for a `pull_request` from a fork is read-only whatever the block says; the deploy runs only by `workflow_dispatch` on `main`, so that does not apply. The token can read the asset, so option (a) stands.

## Options considered

Hosting:

1. **(a) A manually triggered GitHub Actions workflow deploys a site bundle that the builder uploaded as a draft-release asset.** A draft release is visible only to collaborators with push access, so the bundle is never public until Pages serves it, and nothing derived from the IPA enters git: no branch, no commit, no history to rewrite on a takedown. Costs: the workflow needs `contents: write`, and a deploy is two steps (upload, then trigger). **Chosen** (Joris, 2026-11-06), because nothing derived from the IPA is ever committed.
2. **(b) An orphan `gh-pages` branch, replaced by one fresh commit per deploy.** Simple and well known. The derived site would sit in a git branch of this repository, reachable by clone and kept by every fork. Rejected.
3. **(c) A separate repository for the site.** Keeps the derived files out of this repository, but still commits them to git, splits the project and adds a second repository to take down. Rejected.

Content was decided by Joris on 2026-11-05 (everything that serves the compendium, including full dialogue transcripts) and is not reopened here.

## Decision

### Content

The site may publish:

- facts from the data tables, maps, map objects and scripts (stats, drops, prices, spawn points, portals, quest conditions and rewards);
- item, skill and UI icons;
- monster, NPC and hero sprites, as stills and animations;
- map renders;
- full dialogue transcripts in English and Korean, verbatim;
- quotes of item and skill descriptions, verbatim.

The site never publishes:

- audio files: music and sound effects are named and described, never published;
- the executable or any code derived from it (generated C, disassembly listings, reconstructed functions or scripts);
- raw bundle files for download, or a "download the assets" bundle of any kind;
- anything that helps obtain or decrypt the IPA.

### Name and branding

The site is the **MapleThief Compendium**. "MapleStory" is used only descriptively ("a compendium of MapleStory Thief Edition"). No Nexon logo, no Nexon branding, no game logo as the site's mark. Every page carries this footer, word for word (the site generator and every section copy it):

> MapleThief Compendium is an unofficial fan project, not affiliated with or endorsed by Nexon. MapleStory and all game content belong to Nexon. [About this site](https://github.com/armaatus/maplethief-port/blob/main/docs/legal.md#the-compendium-site)

### Build and deploy

- The generator is our code and lives in the repository (`tools/mtport/compendium/`, `scripts/compendium`). Hand-written prose lives in `docs/compendium/` and refers to game entities by `{{kind:id}}` placeholders, so no game text is committed; the generator fills them in from the builder's own copy.
- The site is built locally from `work/ipa/` into the git-ignored `build/compendium/`. It never lands in git, on any branch.
- Hosting is option (a). The builder packs `build/compendium/site/` into one archive and uploads it as the asset of a draft release reserved for the site, then triggers the deploy workflow by `workflow_dispatch`. The workflow (permissions `contents: write`, `pages: write`, `id-token: write`) lists the releases, picks the draft, downloads the archive by asset id, unpacks it and deploys it with GitHub's Pages actions. The Pages artifact is kept for one day. Pages is served from GitHub Actions, not from a branch.
- The site's URL is https://armaatus.github.io/maplethief-port/.

### Takedown plan

On a notice from Nexon or a request on their behalf, the maintainer unpublishes the site within a day: disable Pages, delete the draft release with the bundle, and delete the deploy runs' artifacts. The source, the generator and the hand-written prose stay; they contain no game content. The notice and the response are recorded in an issue.

## Consequences

- Anyone with their own copy can rebuild the site; the repository stays free of game content, and so does its history.
- A deploy needs a maintainer with push access: upload the bundle, then run the workflow. Joris switches Pages on and approves the first deploy (#226).
- The site is the one place this project publishes the game's art and text. That is a larger risk than ADR 0100's: the site is a takedown target in its own right. The takedown plan keeps the response small, and the repository and releases are unaffected by it.
- `docs/legal.md` gains a section on the site, `CLAUDE.md` says the compendium output never lands in git, and [ARCHITECTURE.md](../ARCHITECTURE.md#compendium-site) describes the pipeline.
- **How we'd know this was wrong:** a notice from Nexon, or a change in how comparable fan sites are treated.

## Addendum: the deploy as built (#226, 2026-11-06)

The details option (a) left open, settled when `scripts/compendium` and `.github/workflows/compendium.yml` were written:

- **One draft release, one asset.** The release is tagged `compendium-site` and stays a draft; the asset is `compendium-site.tar.gz`, replaced on every deploy (`gh release upload --clobber`). `deploy` and the workflow both refuse when a published release carries the tag, since the bundle would then be public outside the site.
- **The archive is deterministic** (sorted, undated, owner 0, mode 0644, no hidden files), so its SHA-256 identifies the site. `deploy` passes it to the workflow as the `sha256` input, and the workflow refuses an asset with another digest: a stale or swapped upload is never deployed.
- **What may be published is an allow-list of file types**: HTML, CSS, JavaScript, PNG, SVG, JSON, text and XML. Audio, raw bundle files and anything executable are refused by `check`, by `deploy` and again by the workflow before the Pages artifact is made.
- **The workflow checks nothing out**, so it runs no code of the repository and fetches no Git LFS object; its only inputs are the asset and the digest.
- **The guard** (`scripts/compendium guard`) fails CI's lint and Python job and `scripts/local-ci` on any tracked file under `build/`, `work/` or `assets-out/`, a site marker, search index, image manifest or bundle, a page carrying the site's footer, or an image outside `tests/golden/data/` and `docs/controls/`. A new tracked image directory has to be added to its list on purpose.
- **Search engines:** the site has a sitemap, Open Graph title and description (no images), and a 404 page with a `<base>` element and `noindex`. `robots.txt` is written too, but crawlers read it only at a host's root, so it matters only under a custom domain.

## Addendum: deploy on merge (#248, 2026-11-06)

Joris asked that merging compendium changes into `main` redeploys the site without a manual `scripts/compendium deploy`. CI still never sees the IPA (ADR 0100), so CI cannot rebuild the knowledge base or the images. Most compendium changes touch only the generator, the sections, the templates or the prose, and those need only the knowledge base and the images. Both are derived data of the same kind as the site bundle that already sits on the draft release.

- **The inputs bundle.** `scripts/compendium deploy` also packs `build/compendium/kb/*.json` and `build/compendium/site/img/` into `compendium-inputs.tar.gz` (deterministic, like the site bundle) with an `inputs.json` inside, and uploads it to the same draft release with its metadata `compendium-inputs.json` (SHA-256, size, fingerprint). The upload order is inputs, metadata, site, and only then the workflow run, so a half-done upload fails the SHA-256 check instead of pairing new metadata with an old bundle. The bundle holds nothing the site does not publish already (the knowledge base is the site's data in another shape, the images are the site's images), it stays on a draft, and it never lands in git: the guard refuses both file names and any tracked JSON shaped like a knowledge-base file.
- **The extraction fingerprint.** A SHA-256 over the repository-relative path and SHA-256 (line endings normalised to LF) of every module of `tools/mtport` that `mtport.compendium.kb` and `mtport.compendium.images` import, directly or transitively, read from the import statements (also those inside functions, which is how `kb.py` loads its stages), plus the packages' `__init__.py` files and a version string. Today that is 33 files: the KB stages (`tables`, `world`, `quests`, `scriptwalk`), `images`, and the parsers and renderers they use. `scripts/compendium fingerprint --files` lists them. The site's own code (`site`, `siteapi`, `defaults`, `templates`, `assets`, `sections/`, `cli`, `bundle`, `inputs`, `htmlpage`, `prose_lint`) is not hashed. Computing the list from the imports, instead of writing it down, keeps it right when a stage gains an import; it errs towards hashing too much, which only costs a manual deploy.
- **The push trigger.** `.github/workflows/compendium.yml` also runs on a push to `main` that touches `tools/mtport/compendium/**`, `tools/mtport/prose_lint.py`, `tools/mtport/htmlpage.py` (the generator's HTML helpers), `docs/compendium/**`, `scripts/compendium` or the workflow itself. A test checks that every module the site build imports and the fingerprint does not hash is covered by these paths. The push run makes a sparse checkout of `tools/mtport/`, `docs/compendium/` and `scripts/compendium` (no Git LFS, no credentials kept), downloads the inputs bundle and its metadata by asset id, and runs `scripts/compendium from-inputs` with the runner's own `python3` (standard library only, no package install): it checks the bundle against the metadata, compares the fingerprint with the checkout's, unpacks it (plain files at the allowed paths only), builds the site, runs the site check (links, completeness, prose lint, footer, fresh-build equality) and the bundle check, and packs the site. The same check-and-unpack step and the same Pages actions as a manual deploy follow. The dispatch path is unchanged: no checkout, no code of the repository run.
- **A stale fingerprint is a skip, not a failure.** When the pushed code has another fingerprint than the bundle, the knowledge base or the images could differ, so the run writes a `::warning::` and a job summary naming the changed files ("run `scripts/compendium deploy` locally"), skips the Pages steps, and ends green. The live site stays as it was until the maintainer deploys.
- **Trust.** A push run executes the pushed generator with `contents: write` and the Pages permissions. Only commits on `main` reach it (`github.ref` guard, push trigger limited to `main`), which is the same trust as the maintainer running `deploy`.
- **Takedown.** Deleting the draft release now removes the inputs bundle as well. Disable the workflow too (`gh workflow disable compendium.yml`), so a later compendium merge does not fail looking for the release.
- **Consequences.** A compendium merge that changes only the generator or the prose goes live by itself in about a minute of CI. A merge that changes extraction code needs `scripts/compendium deploy` as before, and the warning says so. The first push run needs an inputs bundle on the draft release, so the maintainer runs `scripts/compendium deploy` once after this change merges.
