<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Original neutral podcast tiles

The public source contains original procedural sound-wave graphics under the
repository MIT license. They do not reproduce a podcast publisher's cover.
`manifest.json` declares `artwork_policy: original-neutral`, has an empty
`artworks` collection, and records RGB565 hashes under `neutral_tiles`.
Stable source IDs, the 112 × 112 and 48 × 48 descriptor API, aligned immutable
pixels, and the original 386,048-byte ROM budget are preserved.

Generate from the repository root, without network access or third-party image
libraries:

```bash
python3 firmware/tools/generate_podcast_covers.py
```

This regenerates `firmware/main/podcast_covers.c`, the manifest, and new
`assets/publication/neutral-covers/neutral-<id>-<size>.png` previews. It never
reads or overwrites private publisher originals. Historical `originals/` and
`<id>-<size>.png` assets may remain locally for the owner's personal use;
they are excluded from public source and release archives.

The current production UI displays dynamically delivered 52 × 52 covers.
These embedded neutral descriptors remain compatible resources, but do not
establish dynamic artwork delivery or physical-device rendering. Publication
screen exports use original sample tiles and fictional show/episode names.
