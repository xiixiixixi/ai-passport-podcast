<p align="right"><a href="podcast-player.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Three-button podcast player

An independent application for the AI Passport's 240×320 display. Audio retains 16kHz, 16-bit mono and the established gain; it never hides starvation by changing speed or discarding samples.

The playback page follows the user's selected radio reference: a small 52px cover, two-line Chinese title, prominent elapsed clock, truthful time ruler, numeric volume and ten volume segments. White, black and cobalt blue establish the hierarchy. Pause, preparation and failure states retain explicit actions; position editing and the original physical-key operations remain separate from the new presentation.

## Controls

| Page | Up / Down click | OK click | Side-key hold | OK hold |
| --- | --- | --- | --- | --- |
| Shows | Select a show | Open episodes | Up: latest show; Down: final show | Return to current playback or resume recent listening |
| Episodes | Select, paging at boundaries | Resume the selected bookmark | Up: reverse date order, preserving episode ID; Down: first in this order | Return to shows |
| Playback | Volume up / down by 5 | Pause / resume, retry an error, replay completion | Open position adjustment | Open actions |
| Position | Target backward / forward 15s | Apply one seek and restore prior playback state | Target backward / forward 60s | Cancel and restore prior playback state |
| Actions | Select | Execute | No extra action | Return |
| Sleep | Off / 15 / 30 / 60 minutes | Apply | No extra action | Cancel and return |

Press-down does nothing; double click means one click. No chords are required. Up increases volume by 5 and Down decreases it by 5, including the volume adjustment page; selection and position adjustment keep their existing directions. Position adjustment pauses audio; previewing does not repeatedly reconnect. Volume commands bypass catalogue requests and display reads the audio snapshot. The service defers volume persistence instead of writing storage inside a button callback.

The playback footer keeps one quiet instruction: hold a side key to adjust position. Other control instructions appear in the reserved footer for about three seconds on entering a page, then disappear. List pages wait until their first usable content arrives before starting this period. Selection changes, playback progress and battery updates do not restart it. Leaving a page cancels its hint timer; returning shows the instructions again. The position editor keeps a brief apply/cancel instruction, and the volume editor keeps its return instruction. No extra key gesture is introduced.

## Ordering and next episode

Public firmware obtains its network and backend through [connection setup and device pairing](podcast-setup.md). The last playback action opens connection settings; existing controls and listening records remain intact.

Shows sort by the actual newest publication timestamp, with unknown dates last. A distinct recent-listening block appears above the catalogue when available; it does not replace a subscribed show. OK hold still reaches current/recent playback. Episodes default to newest first; reversing order keeps the same selected episode ID. Only three episodes load at once, with date, direction and list position visible.

Starting an episode freezes its time direction. Autoplay defaults on: newest-to-oldest chooses the adjacent older item in the same show, and oldest-to-newest chooses the adjacent newer item. Equal publication times use stable source order; unknown dates remain at the end and never borrow fetch time. Subsequent browsing does not change the active playback direction.

The active playback sequence retains its next title and direction while the focused radio screen prioritises the current episode. It requests background preparation of the next episode after starting the current one, retrying temporary submission failures periodically. The final item stops, without wrapping or crossing into another show. Actions can disable autoplay, select previous/next, restart or locate the current episode. Sleep expiry takes precedence over completion and suppresses delayed preparation starts.

## Continuous audio

Startup and roughly three-hour catalogue refreshes prepare each show's latest episode. An uncached selection downloads and prepares the complete episode before playing. A reserved foreground slot protects user selections; next-episode prefetch outranks ordinary background jobs. Readiness requires every device-format segment and its source manifest to be valid. Existing complete compressed segments are reused, repairing only absent or invalid device audio. Playback requests never invoke conversion.

Storage remains divided into approximately five-minute files, while one logical response streams the entire episode. Exact segment lengths map bookmarks to a global byte range. Crossing a file boundary does not clear queues, drain before reconnecting, duplicate samples or omit samples. The network queue is 32KiB, about 1.024s, with 24KiB initial prebuffer, about 0.768s; hardware retains about 240ms more. An episode is not fully downloaded to the device; an outage exceeding bounded storage still requires explicit rebuffering.

Ten-second diagnostic records expose buffered duration, underruns, crossed segments and heap/stack resources. They support device acceptance without substituting for listening. Lightweight details omit the complete URL array so long episodes remain within device memory.

## Bookmarks and lifecycle

Retain 24 recent episode records and volume. Checkpoint on pause/change/completion and every 60s during continuous playback. Hard power loss may return to the last checkpoint, up to about one minute. Complete identifiers bind exact segment/byte positions, not a download percentage. Conservative pause accounting may replay up to about 240ms, preventing missing speech. Existing record version, length and checksum remain unchanged; damaged records are skipped without erasing storage.

The server also holds shared episode state, listening time, recent listening and per-show statistics. Resume checks the central revision before using a position. An eight-session persistent device outbox retains immutable unacknowledged events; retries must not create a second listening record. Only audio actually consumed contributes listening time. Silence during mute, buffering, pause and seeking do not fabricate heard time. Importing old bookmarks preserves positions without inventing historical time or play counts. Actual device/web handover remains a separate acceptance step.

Completion is emitted only after the complete response, queue and hardware tail finish. Playback session and completion identifiers reject stale finish states when restarting or seeking. Network cancellation requires acknowledgement; unacknowledged tasks are not forcibly deleted.

## Presentation and acceptance

The selected reference is retained at `assets/images/podcast-radio-selected.png`; it is a design target, not a full-screen runtime bitmap. Real artwork is drawn into a 52px slot. The 48px elapsed clock falls back by measured width for long hour-formatted values. The ruler maps the actual duration to every tick and pointer, switching between seconds, minutes and hours; unknown duration has no fabricated scale. Volume 55 displays five full segments and a half segment. The 18px Chinese font and the actual physical-key actions remain intact.

New-source artwork synchronizes automatically from the paired backend. Existing server image tooling prepares and retains fixed 52×52 images and their content versions; the device only accepts the exact length, dimensions and validated checksums, without accessing third-party artwork servers. Three visible slots and one pending image consume at most 21,760 image bytes, plus a low-priority task and bounded HTTP overhead. Images are allocated on demand and optional caches are released before playback starts, without shrinking audio buffers or writing progress/partitions. Stale page requests cannot replace another show's image. Slow networks, low memory and pending preparation retain embedded or neutral fallback artwork until bounded retries succeed. Physical image delivery and audio continuity still require device acceptance.

The full repository gate proves compilation and host checks, not listening or physical appearance. Production-widget previews check typography, long titles, volume limits, state changes, truthful ruler geometry and the 24KiB UI pool. The separate 40-line RGB565 display buffer consumes 19,200 bytes; host-pool results do not establish whole-device audio/network memory. Device checks separately cover cross-segment continuous audio, underruns and contiguous internal memory, volume priority, position apply/cancel, automatic next, sleep and networking recovery. Flash only the verified compatible application partition, retaining settings/bookmarks and avoiding chip erase. Exact artifact identity and actual device evidence belong in the local delivery record.
