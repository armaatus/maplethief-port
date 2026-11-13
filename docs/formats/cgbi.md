# CgBI PNGs: Apple's "crushed" iOS images

**Status: fully specified.** All 113 shipped CgBI files decode, convert to standard PNGs, and match macOS ImageIO (`sips`) in every channel of every pixel. Issue [#24](https://github.com/armaatus/maplethief-port/issues/24).

CgBI is not a format the game's developers chose. When Xcode builds an iOS app it runs `pngcrush -iphone` over the PNGs in the bundle's resource list and rewrites them into a variant that the iPhone's image stack can upload without conversion: pixels in BGRA order with premultiplied alpha, and a deflate stream without the zlib wrapper. Only Apple's ImageIO reads the result; a standard decoder (libpng, browsers, Pillow) rejects it. The variant is publicly known; this page records it as the bundle uses it.

## Where the game loads it

The CgBI files are the images of the UIKit shell (main menu, character select, touch keypad, popups, market), loaded by name through `+[UIImage imageNamed:]` (80 call sites, [re/imports.md](../re/imports.md)) in classes such as `MainMenuView`, `TouchKeyPadView` and `ChooseCharacterView`, plus the app icons that SpringBoard draws. The game engine's own images (`png/`, `sprite/`, `map/`) were copied as folder references, which Xcode leaves alone, so they are standard PNGs and go through `CMImage::Load` (`0xBB49`, [png-dat.md](png-dat.md)) instead. No guest code parses CgBI: decoding happens inside ImageIO, which the runtime replaces with stb_image ([ADR 0003](../adr/0003-dependencies.md)).

## Files

113 of the 1,322 `.png` files: **112 at the bundle root** and **1 in `English.lproj`** (`v1.0.0.png`). The other three root PNGs (`Default.png`, `npcAcceptButton_kor.png`, `npcRefuseButton_kor.png`) are standard ones, so files are told apart by content, never by folder. None of the PNGs under `png/`, `sprite/` or `map/` (nor the 473 PNGs inside the `png/**/*.dat` packs) are CgBI.

Every CgBI file in the bundle is **8-bit, colour type 6** (BGRA), non-interlaced, 28–580 pixels wide and 14–665 high, with one `IDAT` chunk and **filter type 1 (Sub) on every scanline**. Their ancillary chunks are left over from the source files: `iCCP`, `cHRM` and `pHYs` (Photoshop exports) in most, `tEXt` in 40, `gAMA` in 1.

## Layout

A CgBI file is a PNG (ISO/IEC 15948) with four differences. All integers are big-endian, as in PNG; every chunk, `CgBI` included, carries a normal CRC-32 over type and payload, and all 113 shipped CRCs are valid.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | signature | `89 50 4E 47 0D 0A 1A 0A`, as in PNG |
| 8 | 16 | **`CgBI` chunk** | length 4, type `CgBI`, payload, CRC. Always the first chunk, before `IHDR` |
| 24 | 25 | `IHDR` | as in PNG. Colour type 6 means B, G, R, A here |
| 49 | … | ancillary chunks | as in PNG; they don't affect decoding |
| … | … | `IDAT`… | concatenated payloads are a **raw deflate stream** (RFC 1951): no 2-byte zlib header, no Adler-32 trailer |
| … | 12 | `IEND` | as in PNG |

### `CgBI` payload

| Byte | Shipped values | Meaning |
|---|---|---|
| 0–1 | `30 00` | constant in all 113 files; meaning not documented by Apple. Not needed: no decoder reads it |
| 2 | `20` | constant; meaning not documented. Not needed: no decoder reads it |
| 3 | `02` (105 files), `06` (8 files) | `06` in exactly the 8 fully opaque files (every alpha 255): `ChooseCharacterBackground`, `ChooseCharacterStatusScrollTable`, `MainMenuBackground`, `MainMenuCompanyLogo`, `MainMenuHelp`, `MainMenuHelp_kor`, `marketBuyTab`, `marketSellTab`; `02` in all 105 files with any alpha below 255. That is an observed rule over the whole bundle, not Apple's definition. Not needed: decoders ignore it |

### Pixels

After inflating, the data is ordinary filtered scanlines (`height` rows of one filter byte + `width × 4` bytes) and the five PNG filters apply unchanged: they work on bytes at a distance of 4, so the channel order doesn't matter to them. Each unfiltered pixel is

```
B G R A     with B, G, R premultiplied by A:  stored = round(straight × A / 255)
```

So a standard RGBA pixel is recovered by swapping bytes 0 and 2 and un-premultiplying:

```
c = min(255, (c * 255 + A / 2) / A)    integer division, for each of R, G, B, when A > 0
(0, 0, 0, 0)                            when A = 0
```

That is stb_image's rounding (`stbi__de_iphone`), and it reproduces `sips` exactly on every shipped file. In the shipped files no colour byte exceeds its alpha and no transparent pixel stores a colour, so the clamp and the `A = 0` rule never change a value there; they are only there for malformed input. Low alphas lose colour precision through premultiplication (at `A = 1` a channel can only be 0 or 255 afterwards); for `A ≥ 128` the round trip straight → premultiplied → straight is within ±1.

A CgBI file with colour type 2 (BGR, no alpha) would only need the swap; none ships.

## Worked example

`marketBuyTab.png` (115×26, 3,415 bytes, one of the opaque files):

```
0x00  89 50 4E 47 0D 0A 1A 0A                       signature
0x08  00 00 00 04  43 67 42 49  30 00 20 06  17 9E 80 65   length 4, "CgBI", payload, CRC
0x18  00 00 00 0D  49 48 44 52  00 00 00 73  00 00 00 1A   IHDR: 115 x 26
0x28  08 06 00 00 00  84 B8 63 74                    depth 8, colour 6, CRC
0x31  iCCP (2,633 bytes), 0xA86 cHRM, 0xAB2 pHYs
0xAC7 00 00 02 71  49 44 41 54  ED 98 31 6F ...      IDAT, 625 bytes: raw deflate
                                                     (a zlib stream would start 78 xx)
```

The inflated stream starts `01  63 65 63 FF  39 33 39 00 …`: filter Sub, first pixel B, G, R, A = `63 65 63 FF`, second pixel `63+39, 65+33, 63+39, FF+00` = `9C 98 9C FF`. With R and B swapped and alpha 255, the RGBA pixels are `63 65 63 FF` and `9C 98 9C FF`.

## Parser

[`tools/mtport/cgbi.py`](../../tools/mtport/cgbi.py), on the standard-library PNG codec [`tools/mtport/png.py`](../../tools/mtport/png.py) (ADR 0010):

- `cgbi.is_cgbi(data)`, `cgbi.decode(data, name, premultiplied=False) -> png.Image` (straight RGBA8 by default; the premultiplied mode keeps the stored values, R and B swapped, for direct GPU upload, #57), and `cgbi.convert(data, name)`, which returns a deterministic standard 8-bit RGBA PNG (`IHDR`, one `IDAT`, `IEND`; the ancillary chunks are dropped) and returns any non-CgBI PNG **byte-identical**.
- `python3 -m mtport.cgbi convert IN... --out DIR [--premultiplied]` converts files or directories (searched for `*.png`, relative paths kept). `python3 -m mtport.cgbi check [DIR]` converts every PNG under DIR (default: the bundle) in memory, re-decodes each result and checks that standard PNGs pass through unchanged.
- `mtport.formats` decodes every `cgbi-png` file with it.

## Validation

- **CI** (`tests/test_cgbi.py`, synthetic): an encoder in the test crushes known straight-alpha images (premultiply, BGRA, all five filters, raw deflate). Decoding them gives the original within ±1 per channel (alpha ≥ 128 or 0), exactly for opaque images, and the premultiplied mode gives the stored values exactly. Standard PNGs pass through as the same object. The codec has its own tests for every colour type, bit depth and filter (`tests/test_png.py`).
- **Local, real assets** (`tests/test_cgbi_real.py`): 113/113 CgBI files convert; each output inflates with plain `zlib.decompress` (zlib header and Adler-32 checked) and decodes with `mtport.png` to the un-crushed pixels. On macOS, `sips` decodes each original to **exactly** our pixels (maximum channel difference 0) and opens each converted file to the same pixels. All 1,209 standard `.png` files pass through byte-identical, and they and the 473 PNGs in the `.dat` packs all decode with `mtport.png`. The flag/opacity, filter and premultiplication facts above are asserted there too.
