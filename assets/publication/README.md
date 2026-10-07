[简体中文](README.zh_CN.md) · **English**

# Publication artwork for 随身听

| File | Dimensions | Purpose |
| --- | --- | --- |
| `player-native.png` | 240 × 320 | Lossless production-player screen export |
| `library-native.png` | 240 × 320 | Lossless production-library screen export |
| `play-cover-3x4.png` | 1200 × 1600 | Play-site portrait cover |
| `library-showcase-3x4.png` | 1200 × 1600 | Play-site library illustration |
| `github-showcase.png` | 1600 × 900 | Repository overview illustration |

The two native screens are rendered on a computer by the actual
`firmware/main/podcast_ui.c` implementation, with LVGL 9.5.0 and the same
240 × 320 RGB565 display format and 40-line partial buffer used by the device.
They are **host-rendered exports, not device captures or photographs**.
Show names, episode titles, dates, playback position and battery state are
fictional samples. No listening history, relay configuration, credentials or
personal screenshots are used.

Original sound-wave tiles are generated without image inputs or network access
by `firmware/tools/generate_podcast_covers.py`. The preview adopts three sample
52 × 52 PDC1 wires through the production size, CRC and SHA validation and
registry path. Its host-only adoption entry is excluded from device compilation;
no production download success is fabricated. This verifies screen rendering
with sample imagery, not hardware delivery, audio playback or live networking.

`render.py` losslessly encodes the PPM output as PNG and creates the three
original SVG layouts. `render.mjs` rasterizes those layouts with
`@resvg/resvg-js` 2.6.2. The screen images remain intact inside the SVG, enlarged
by an integer factor of two. No screen content is redrawn by an image model.
There are no product-shell renders, official brand reference images, publisher
artwork, third-party photographs or QR codes. Original publication graphics
use the repository MIT license. The Noto Sans SC font retains its SIL Open
Font License 1.1 notice at `firmware/assets/fonts/LICENSE-NotoSansSC.txt`.

After preparing the pinned firmware dependencies, reproduce from the repository
root:

```bash
python3 firmware/tools/generate_podcast_covers.py
cmake -S firmware/tools/podcast_preview -B firmware/tools/podcast_preview/build
cmake --build firmware/tools/podcast_preview/build --target preview -j 4
cd assets/publication
../../firmware/tools/podcast_preview/build/preview --publication
cd ../..
python3 assets/publication/render.py
npm install --prefix /tmp/passport-publication-render --no-audit --no-fund @resvg/resvg-js@2.6.2
NODE_PATH=/tmp/passport-publication-render/node_modules node assets/publication/render.mjs
```

The publication preview checks actual font glyphs, widget bounds and native
cover pixels before reporting success. Both native screens and all three final
PNG layouts were visually inspected. This material does not claim physical
acceptance of the latest public firmware. `provenance.json` records the source
and output hashes; the paired README files state the evidence boundary.
