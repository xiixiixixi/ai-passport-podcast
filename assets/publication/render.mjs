/** Rasterize original publication SVG layouts, preserving the embedded production UI. */
import { createRequire } from 'node:module';
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
const require = createRequire(import.meta.url);
const { Resvg } = require('@resvg/resvg-js');
const here = dirname(fileURLToPath(import.meta.url));
const font = join(here, '../../firmware/assets/fonts/NotoSansSC-Regular.otf');
for (const name of ['play-cover-3x4', 'library-showcase-3x4', 'github-showcase']) {
  const renderer = new Resvg(readFileSync(join(here, `${name}.svg`)), {
    font: { fontFiles: [font], loadSystemFonts: false, defaultFontFamily: 'Noto Sans SC' },
  });
  writeFileSync(join(here, `${name}.png`), renderer.render().asPng());
  console.log(`${name}.png`);
}
