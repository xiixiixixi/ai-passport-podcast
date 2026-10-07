[简体中文](README.zh_CN.md) · **English**

# Pocket Podcast · 随身听

Turn AI Passport into a three-button podcast player. Choose a show, listen to an episode, and resume where you paused. Manage subscriptions, audio caches and listening progress on your own household server.

![Pocket Podcast player and features](assets/publication/github-showcase.png)

[Download the installation package](https://github.com/xiixiixixi/ai-passport-podcast/releases) · [Install and pair](docs/install.md) · [User guide](docs/use.md)

## A player you can read at a glance

A white-and-blue time ruler, large elapsed clock and volume segments make playback progress clear. Browse shows by their latest update, open their episode lists, or continue a recent listen.

| Player | Library |
| --- | --- |
| ![Player](assets/publication/player-native.png) | ![Library](assets/publication/library-native.png) |

These images are exported on a computer by the actual UI implementation with fictional sample programmes and original artwork. They show the layout; they are not device photographs or personal listening records.

## Everyday listening

- Pause and resume, change volume, and seek in 15- or 60-second steps.
- Continue to the next episode in the selected direction; set a 15-, 30- or 60-minute sleep timer.
- Save recent positions and share progress between the web player and paired devices.
- Search or paste a link to add shows, and manage household subscriptions and devices.
- Pre-cache the latest three episodes per show by default. View disk use and adjust prefetch and cleanup retention.
- Let the screen turn off while audio continues; the first key gesture wakes it.

## Start with your own server

The product includes device firmware and a self-hosted backend. One household server can authorize several players sharing that household's shows and progress. The server must stay on while listening.

1. Install and start Docker on a computer or household server.
2. Download and unpack the installation package. On macOS, open `install.command`; use the matching script on other systems as described in the [installation guide](docs/install.md).
3. Open the first-setup page, set an administrator password, and generate a six-digit pairing code in My Devices.
4. After installing compatible firmware, connect your phone to the device's setup hotspot and enter your Wi-Fi network, backend address and pairing code.
5. Successful network verification and pairing open the library. Subsequent startups go directly to listening.

Use a backend LAN address reachable from the device's 2.4 GHz Wi-Fi network. Public source requires web sign-in and device pairing; it excludes the author's private passwordless patch.

## Check compatibility before installation

This public preview targets ESP32-C3 devices with 8 MB flash and an initialized modern permanent-Recovery system. For older or unknown layouts, first read the [installation guide](docs/install.md) and [recovery compatibility](firmware/docs/podcast-recovery.md). Do not write a complete image blindly.

Release assets include a complete image and an application upgrade image. The complete image is intended for offset zero; the app-only image must use the upgrade tool's configured offset. The package contains no permanent-Recovery binary and provides no old-device identity migration. First installation and the new pairing flow still require further on-device acceptance. See the [validation record](docs/validation.md).

## For developers

| Directory | Contents |
| --- | --- |
| `firmware/` | Player, board support, fonts and tests |
| `server/` | Web interface, feeds, cache management and device access |
| `scripts/` | Backend installation, protected upgrades and public export |
| `assets/publication/` | Original artwork, actual UI exports and reproducible tools |
| `docs/` | Installation, usage, validation and licensing boundaries |

Application code uses MIT, retaining the original board-support license and the font and icon notices. See [third-party materials](docs/third-party.md). Public packages exclude household configuration, credentials, QR secrets, listening records, audio caches, internal repair logs and programme artwork without confirmed redistribution permission.
