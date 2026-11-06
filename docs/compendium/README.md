# Building and deploying the compendium

The MapleThief Compendium is built on a machine that has its own copy of the game in `work/ipa/` (from `scripts/bootstrap`), checked there, and published to https://armaatus.github.io/maplethief-port/ by a workflow that `scripts/compendium deploy` starts, and that a merge of compendium changes into `main` starts by itself ([below](#deploy-on-merge)). [ADR 0121](../adr/0121-public-compendium-site.md) has the reasons; this page has the steps. The writing rules for the prose are in [style.md](style.md), the knowledge base is described in [kb.md](kb.md).

Everything below goes through `scripts/compendium`, which runs `python3 -m mtport.compendium.cli` (Python 3.10 or newer; `PYTHON=` picks another interpreter).

## Build

```sh
scripts/bootstrap            # once: your own IPA into work/ipa/
scripts/compendium build     # knowledge base, images, site
```

`build` runs three steps, each of which can also run on its own:

| Step | Command | Writes |
| --- | --- | --- |
| Knowledge base | `PYTHONPATH=tools python3 -m mtport.compendium.kb build` | `build/compendium/kb/` |
| Images | `PYTHONPATH=tools python3 -m mtport.compendium.images build` | `build/compendium/site/img/` |
| Site | `PYTHONPATH=tools python3 -m mtport.compendium.site build` | `build/compendium/site/` (all but `img/`) |

The site step also writes `404.html`, `robots.txt` and `sitemap.xml`. Every step is deterministic: the same copy of the game gives the same bytes.

## Check

```sh
scripts/compendium check
```

`check` runs every step's own check and stops nothing early, so one run lists every problem:

- the knowledge base against a rebuild, plus its relation checks;
- the images against their manifest, and every image byte for byte against a fresh build;
- the site: every internal link and anchor resolves, every search entry has its page, every entity has a page or an anchor, the prose lint finds nothing, every page carries the disclaimer footer, and every file equals a fresh build;
- the bundle: only the file types the site publishes (HTML, CSS, JavaScript, PNG, SVG, JSON, text and XML), no symlinks, a home page and a 404 page;
- the guard (below) on this checkout;
- the size report: the total, each directory, and the ten largest files. A site over the 1 GB GitHub Pages limit fails.

`scripts/compendium size` prints the size report on its own. Images are lossless PNG and APNG with adaptive row filters; the map renders at 2x make up most of the total.

## Preview

```sh
scripts/compendium serve     # http://127.0.0.1:8000/maplethief-port/
```

`serve` serves the site under the same `/maplethief-port/` sub-path as GitHub Pages, and answers a missing page with `404.html`. The 404 page points its links at the live site (a `<base>` element, since Pages serves it at any depth), so on the preview its own links lead to the live site. The site also opens straight from `build/compendium/site/index.html`.

## Deploy

```sh
scripts/compendium deploy --dry-run   # everything but the upload
scripts/compendium deploy
```

`deploy` needs `gh`, logged in as a maintainer with push access. In order, it:

1. runs `scripts/compendium check`, and refuses to go on when it fails;
2. packs `build/compendium/site/` into `build/compendium/compendium-site.tar.gz` (sorted, undated, so the same site gives the same archive);
3. packs the knowledge base and the images into `build/compendium/compendium-inputs.tar.gz`, with the extraction fingerprint of your checkout, and writes its metadata `compendium-inputs.json` (`scripts/compendium pack-inputs` does this step on its own);
4. prints what it publishes: the size report, both archives' sizes and SHA-256, and the fingerprint;
5. asks for confirmation (`--yes` skips the question);
6. finds the draft release `compendium-site` by listing the releases (a draft is never found by its tag), creates it as a draft when it does not exist, and refuses when a published release has that tag;
7. uploads the inputs bundle, its metadata and the site archive to it, replacing the previous ones;
8. starts [the deploy workflow](../../.github/workflows/compendium.yml) on `main` with the site archive's SHA-256.

The workflow checks nothing out and builds nothing. It downloads the archive by its asset id, checks it against the SHA-256, refuses any path outside the site and any file type the site does not publish, and deploys it with `actions/upload-pages-artifact` (kept one day) and `actions/deploy-pages`. Follow it with `gh run watch` or on the Actions tab.

`--dry-run` stops before `gh` is called and prints the commands it would run. `MTPORT_GH` names another `gh`, which is how the tests stub it.

Deploy from a clean checkout of `main` (or of the branch about to merge): the fingerprint is taken from the files on disk, so uncommitted edits to the extraction code make later push runs skip.

### Deploy on merge

A push to `main` that changes the generator or the prose deploys the site by itself (#248, [ADR 0121's addendum](../adr/0121-public-compendium-site.md#addendum-deploy-on-merge-248-2026-11-06)). The workflow runs on a push that touches `tools/mtport/compendium/**`, `tools/mtport/prose_lint.py`, `tools/mtport/htmlpage.py`, `docs/compendium/**`, `scripts/compendium` or the workflow; nothing else on `main` starts it. It:

1. checks out only `tools/mtport/`, `docs/compendium/` and `scripts/compendium` (no Git LFS);
2. downloads `compendium-inputs.tar.gz` and `compendium-inputs.json` from the draft release;
3. runs `scripts/compendium from-inputs`, which checks the bundle against the metadata's SHA-256, compares the bundle's extraction fingerprint with the pushed code's, unpacks it, builds the site with the pushed generator, runs the site check, and packs the site;
4. deploys it with the same steps as a manual deploy.

The **extraction fingerprint** is a hash of the code that makes the knowledge base and the images: every file of `tools/mtport` that `compendium/kb.py` and `compendium/images.py` import, directly or through each other. `scripts/compendium fingerprint --files` prints the list and the hash. When a push changes one of those files, CI cannot rebuild the knowledge base or the images (it never has the IPA), so the run **skips the deploy**: the job summary and a warning name the changed files and say "run `scripts/compendium deploy` locally". The run stays green and the live site is unchanged until you do.

The same build runs locally without `work/`: `scripts/compendium from-inputs build/compendium/compendium-inputs.tar.gz --meta build/compendium/compendium-inputs.json --root <empty dir> --out <file>` (exit status 3 means the fingerprint differs).

The first push run needs an inputs bundle on the draft release, so run `scripts/compendium deploy` once after #248 merges.

### First deploy

Switching GitHub Pages on is a repository setting, and only a maintainer can change it: Settings, Pages, "Build and deployment", source "GitHub Actions". The first run of the workflow creates the `github-pages` environment; when that environment has required reviewers, the run waits for approval on the Actions tab.

### Takedown

On a notice from Nexon or a request on their behalf, follow ADR 0121's takedown plan: unpublish Pages (Settings, Pages), delete the draft release `compendium-site` with its assets, the site and the inputs bundle (`gh release delete compendium-site`), disable the deploy workflow so merges stop starting it (`gh workflow disable compendium.yml`), and delete the deploy runs and their artifacts. The source and the prose stay; they hold no game content.

## What never goes in git

The site, its images and everything else under `build/compendium/` are derived from the game and stay on the builder's machine. Never commit, on any branch:

- anything under `build/`, `work/` or `assets-out/` (all three are git-ignored; never force-add them);
- the bundles `compendium-site.tar.gz` and `compendium-inputs.tar.gz`, the metadata `compendium-inputs.json`, a knowledge-base JSON file, the search index `search-index.js`, the site marker `.mtport-compendium-site` or an image `manifest.json`;
- a built page, or a copy of one;
- an exported image: a sprite, icon or map render, a screenshot or a crop of one. The only tracked images are the synthetic golden images in `tests/golden/data/` and the drawings in `docs/controls/`.

`scripts/compendium guard` fails on any tracked file of these kinds. CI runs it in the lint and Python job of every pull request, and `scripts/local-ci` runs it on its clean checkout. The prose in `docs/compendium/pages/` refers to game entities as `{{kind:id}}` and never quotes game text, so it is safe to commit.
