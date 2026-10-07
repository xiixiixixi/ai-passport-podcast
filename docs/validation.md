[简体中文](validation.zh_CN.md) · **English**

# Public preview validation — 2026-10-07

This record applies to the rebuilt public firmware with neutral artwork. Automated checks establish only their stated scope; first installation and actual listening acceptance are recorded separately.

| Scope | Result | Evidence boundary |
| --- | --- | --- |
| Complete firmware gate | PASS | Static checks, host tests, independent rebuild, layout and archive identity verification with ESP-IDF 5.5.3 |
| Firmware size | PASS | Application: 5,499,904 bytes, below the 6,291,456-byte app protocol limit. Full image: 5,565,440 bytes, below the device's eight-MiB capacity |
| Complete backend suite | PASS | 147 tests covering real generated audio, ranges, persistence, authorization, pairing races and immediate catalog updates after subscription changes |
| Cache management | PASS | Fourteen tests within that suite cover three episodes per show by default, saved settings, newest-first preparation, on-demand protection, ordinary cleanup, low-space contraction and confirmed clearing |
| Browser program behavior | PASS | 38 tests execute actual page code with simulated browser/network boundaries; they do not establish full browser visual or physical-device acceptance |
| Backend installer | PASS | Thirteen isolated groups cover first/repeated installation configuration, account identity, failure preservation and readiness. Container commands are replaced by test doubles; this is not a new real household installation |
| Public export guards | PASS | Eighteen checks cover archives, manifests, sensitive configuration and public-file scope. Real listening data, device-identification records and private media are excluded |
| Public login and pairing | PASS | The normal backend requires administrator login and device credentials. Authentication bypass is restricted to isolated test instances |
| Original presentation assets | GENERATED | Neutral covers and packaging are original. The browser fallback uses the retained-license headphones icon. Host-rendered UI images are not presented as device photographs |
| Windows runtime | NOT RUN | No corresponding command execution environment was available in this run |
| This public firmware flashed to hardware | NOT RUN | The newly rebuilt neutral-artwork firmware has not been written to a physical device |
| First installation, phone setup and Recovery entry | NOT ACCEPTED | Physical testing on an explicitly compatible layout is still required for provisioning, pairing, button-to-Recovery behavior and data-preservation boundaries |
| New public firmware sound, controls and continuous listening | NOT ACCEPTED | Ear-checked listening and physical interaction remain required. Previous firmware playback logs do not establish this result |

Full-image SHA-256: `8ef5e4cda2b57d174cc67d2c7ca8aae5000cd4a98b7a37a851fd6b18202ef10f`. The installation bundle's own `release/manifest.json` binds the application, layout and full image; verify the corresponding downloaded files.

The backend suite ran in isolated directories with the existing local ffmpeg 9.0.1. Audio checks measure actual duration, loudness, consistency between quiet and loud source recordings, and clipping headroom. No failures remain, and catalog or assertion problems were not attributed to unproven environment differences. Other systems and converter versions require their own verification. Public source includes eleven initial shows; each household manages subsequent subscriptions. An existing household's subscription or cache counts are not fresh-install defaults.

This remains a public preview for explicitly compatible devices. Existing startup, networking and continuous-playback logs establish paths exercised by their corresponding earlier builds. They do not establish that this new image is on hardware, that its sound meets expectations, or that first installation and permanent Recovery passed. Existing household services and test instances remain separate. The public bundle includes no household configuration, device credentials or real listening records.
