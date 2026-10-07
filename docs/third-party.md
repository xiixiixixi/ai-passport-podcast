[简体中文](third-party.zh_CN.md) · **English**

# Third-party materials

Application code is distributed under the root MIT license. Original board/template code retains FoloToy's copyright notice in `firmware/LICENSE`. Managed ESP-IDF dependencies retain their own licenses and are downloaded from the pinned `dependencies.lock` rather than republished as generated dependencies.

Actual firmware SDK, UI, audio-driver, networking and runtime library texts/notices are retained with the bundle in the [firmware dependency license index](licenses/README.md). It also retains built-in Latin fonts/icon glyphs and the GCC Runtime Library Exception. The root MIT license does not replace those dependency terms.

The bundled Noto Sans SC source uses SIL Open Font License 1.1; its retained license is `firmware/assets/fonts/LICENSE-NotoSansSC.txt`. Custom bitmap compression does not change the font license. Tabler interface icons use the retained MIT notice under `server/static/assets/icons/LICENSE`. The embedded minimp3 decoder carries its original public-domain/CC0 notice in its header.

The neutral bundled covers and presentation packaging are original project graphics distributed under MIT. The browser fallback uses the headphones icon with the retained icon license above. Personal podcast artwork, brand backgrounds without confirmed licenses and reference enclosure imagery are excluded from public source and installation bundles.

Podcast names, dynamically fetched cover artwork and recordings belong to their publishers and are not relicensed under this project's MIT license. The backend obtains dynamic covers from publisher sources during actual use to identify corresponding shows; personal audio caches are excluded from source and release archives. Public availability is not a blanket license to republish those media. Confirm rights or obtain permission before distributing them separately.

The modern recovery interface is referenced from the fixed community installer commit `127c56bc97b7ea85bf4bfd2b52ce4e2d80a30566`. The boot hook in this project is independently implemented from ESP-IDF APIs and the documented interface; no permanent-Recovery binary or unlicensed community implementation is included. See [the interface reference](https://github.com/SHLcy/ai-passport-miniapp-installer/blob/127c56bc97b7ea85bf4bfd2b52ce4e2d80a30566/docs/development/engineering/ble-recovery-compatibility.md).
