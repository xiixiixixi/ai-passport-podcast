[简体中文](use.zh_CN.md) · **English**

# Listening and storage

Select a show and episode on the device; the recent card at the top of the library is selectable with up/down and confirm resumes it. If that episode is already playing, confirm returns to its live playback page. Confirm toggles pause/resume. On the playback page, up increases volume and down decreases it; hold either direction button to enter seek mode. In seek mode, up moves backward and down moves forward: a short press changes 15 seconds and a hold changes 60 seconds. Confirm applies the selected position; holding confirm cancels. Holding confirm opens listening options and connection settings. Hints are brief and contextual rather than a permanent strip of every button action.

The browser supports subscription search, adding links, recent listening, progress, statistics and device management. Show and episode lists display cached counts or status. Scrollbars are hidden without disabling scrolling.

New subscriptions automatically receive device-sized artwork prepared by the backend for the library and playback page; no manual image import or firmware rebuild is needed. Prepared covers are retained and rebuilt when the artwork URL changes. The device keeps only a small number of visible images in temporary memory, prioritizing audio. Slow networks and pending artwork show fallback images until automatic retry succeeds; shows without artwork retain their fallback.

The `/api/health` endpoint identifies the running code by release and source fingerprint. A changed timestamp or browser cache does not establish that the backend was upgraded.

The server starts refreshing immediately, repeats every three hours by default, and prepares the latest three episodes of each show, newest first. On the first uncached play, the backend prepares and validates the whole episode before marking it ready; later plays reuse the complete cache. On-demand tasks take priority over waiting prefetch tasks and have a reserved preparation slot; running tasks are not forcibly interrupted. The browser estimates waiting time, which depends on source speed, episode length and the queue. Current hardware displays a preparation message without the browser's estimated minutes. During playback, the device receives the audio stream into its small buffer as it listens. The backend must stay running while the device listens.

Cache management under Add a show → Manage subscriptions selects 1, 3, 5 or 10 prefetched episodes per show and a cleanup minimum of 1, 3, 5, 10 or 30 episodes. It displays ready episode counts, cache size, free disk space and the previous cleanup result. Settings persist in the server data directory across restarts and container updates. Both prefetch and cleanup retention default to three episodes per show.

Automatic cleanup runs daily at 03:10 Beijing time. Ordinary cleanup retains the chosen newest episodes and protects active playback sessions, unfinished episodes, episodes listened to in the last fourteen days, and queued or preparing tasks. If disk space remains below the default 16 GiB threshold afterwards, a second pass retains just the newest episode per show while keeping the same protections. Clean up now uses the same policy.

Clear all cache requires confirmation. It removes audio files while retaining subscriptions, authorization and listening progress; active sessions and queued/preparing episodes are temporarily retained. Unlike ordinary cleanup, it also removes other unfinished episodes' audio, which must be prepared again before the next listen. Cache cleanup does not replace backing up the data directory.

`server/data/` contains subscriptions, listening records and authorization. `server/media/` contains audio cache. Container restart and the normal installer preserve both directories. Stop the service before copying these directories for a consistent backup; keep `server/.env` separately and private. Restoring all three on your own server preserves authorization. Audio cache can be recreated; losing the data directory loses records and authorizations.

Resuming first delivers the device's paused observation before checking whether another device has moved the shared cursor. If synchronization fails temporarily, the current episode keeps its local cursor and listening record; queued observations remain durable and retry after reconnection. The final audio buffer after a stop is credited in full, and retries never count that time twice.

The household is the data-sharing boundary. Every authorized device shares the household library and history; independent homes should run separate servers. Removing a device blocks its future protected requests while retaining history. Browser logout does not log out the device. Administrator-password recovery is documented in [server instructions](../server/README.md).

While a matching episode is playing or buffering, seven seconds without input on settings/library/list pages returns to that live episode without loading a checkpoint or restarting audio. The first settings action, Return to playback, exits directly; holding confirm also returns. Seek drafts are not automatically applied.

After fifteen seconds without input, the backlight turns off and page redraw stops while audio, networking and progress storage continue. Screen off now is a manual action. The entire first key gesture only wakes the screen; subsequent input operates it. Wake redraws live progress. This is screen blanking, not system sleep; battery-life improvement has not been measured.
