[简体中文](README.md) · **English**

# Pocket Podcast · 随身听

A podcast player for AI Passport. Manage your shows on the web, then use the card's three buttons to choose an episode and listen. Pause halfway through and pick up where you left off.

[Download the installation package](https://github.com/xiixiixixi/ai-passport-podcast/releases) · [Install and pair](docs/install.md) · [User guide](docs/use.md) · [Official community play](https://ai-passport.folotoy.cn/plays/1024/)

<p align="center">
  <img src="docs/images/podcast-device.png" alt="Pocket Podcast interface preview on an AI Passport enclosure" width="300">
</p>

<p align="center"><sub>Appearance and interface preview; playback values are illustrative.</sub></p>

## Prepare your shows on the web

Search by show name or paste an RSS feed, Apple Podcasts show link, or publicly accessible free Xiaoyuzhou show link.

![Search for a show or add one by its public feed or share link](docs/images/backend-add.jpg)

The backend prepares the latest three episodes per show by default. Check which episodes are ready, view disk use, and adjust prefetch and retention settings. An uncached episode must finish preparation before playback starts.

Xiaoyuzhou's public pages may expose only part of a show's history. Prefer an official feed when one is available.

## Listen with three buttons

Use up and down to browse shows and episodes, then confirm your choice. During playback, confirm pauses or resumes, while up and down change volume. Hold a direction key to enter seeking: a short press moves fifteen seconds, a long press one minute, and confirm applies the new position.

<table>
  <tr>
    <th>Library</th>
    <th>Player</th>
  </tr>
  <tr>
    <td><img src="docs/images/podcast-library.png" alt="Show library with three publicly available podcasts" width="240"></td>
    <td><img src="docs/images/podcast-player.png" alt="Episode title, elapsed time, blue progress ruler and volume" width="240"></td>
  </tr>
</table>

The screen keeps the cover, title, elapsed time, ruler and volume together. It turns off after fifteen seconds of inactivity while audio continues; the first key gesture wakes it. Set a fifteen-, thirty- or sixty-minute sleep timer to pause playback and stop automatic continuation.

## Resume where you paused

Listen halfway through on the web, pause, and continue the same episode from recent listening on a paired device. Web and device players share progress through your household backend, which can pair multiple devices.

**Device firmware and your own backend work together. Audio caches live on the backend computer or household server, which must stay on and be reachable over Wi-Fi while listening. The device does not currently cache audio for offline playback.**

## Install and pair

1. **Check your device.** This preview targets ESP32-C3 devices with 8 MB flash and an initialized compatible permanent-Recovery system. Read the [installation and compatibility guide](docs/install.md) first if the device has an older or unknown layout.
2. **Start the backend.** Install and start Docker on a computer or household server. Download and unpack `installation.zip` from the release page. On macOS, open `install.command`; use the matching script on other systems. Set an administrator password when prompted.
3. **Install the device program.** Follow the installation method appropriate for your device. It replaces the current play. First installation and upgrades that retain data are different paths; the package includes no permanent-Recovery binary or old-device identity migration.
4. **Pair.** Sign in to the backend and generate a six-digit code in My Devices. Connect your phone to the device's setup hotspot, open the indicated setup page, and enter your 2.4 GHz Wi-Fi network, backend LAN address and pairing code. Successful connection and pairing open the library.

This is still a public preview. First installation, phone setup, pairing, recovery and actual listening need further on-device acceptance, and battery life has not been measured. The images illustrate the interface. See the [validation record](docs/validation.md) and [recovery compatibility](firmware/docs/podcast-recovery.md).

## Code and documentation

The repository includes both `firmware/` for the device and `server/` for the web interface and backend. Installation, upgrade and public export tools live in `scripts/`.

Application code uses MIT; see [third-party materials](docs/third-party.md) for dependency licenses and image credits. Public-file checks exclude private configuration, device credentials, listening records, audio caches and internal material. See [publishing guidance](docs/publish.md).
