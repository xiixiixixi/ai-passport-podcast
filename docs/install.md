[简体中文](install.zh_CN.md) · **English**

# Install and pair

## Why installation replaces the previous screen

The device firmware is a standalone play. Installing it replaces the currently running play. The manufacturer's [play page](https://ai-passport.folotoy.cn/plays/65/) explicitly explains this: use desktop Chrome or Edge, power on the device, connect a data cable, select the device, and wait for writing and restart. The website writes firmware over USB; the previous identity-card interface does not need to keep running.

**A working manufacturer USB installer does not make every package suitable for every existing layout.** This public package targets the modern Recovery layout below. A legacy layout with only `nvs`, `phy_init`, and a large `factory` application has no modern permanent Recovery. An application upgrade on that layout does not validate first installation of this public package. First installation, a data-preserving upgrade, and restoration of factory firmware are separate operations.

## Server

Install Docker with Compose v2. Run the root installer for your platform. The first build downloads Python, dependencies and ffmpeg, so it needs internet access and takes longer than later starts. The installer generates an individual local setup key, starts the backend, waits for its health endpoint, and creates `.local/first-setup.html`. Open it locally and set a password of at least ten characters. Do not send that private page or `server/.env` to anyone.

The normal browser address is `http://SERVER-LAN-ADDRESS:8899`. The installer tries to identify a LAN address; confirm the address displayed in My devices. Loopback addresses work only on the server computer. On Linux/macOS an explicit address can be supplied with `bash scripts/install-server.sh --public-url http://SERVER-LAN-ADDRESS:8899`. On Windows use `-PublicUrl` with the PowerShell script. Reserve the server's address in your router. The device requires 2.4 GHz Wi-Fi.

Repeat the installer to start the same installation; it preserves its key, port and mounted data. Updating an old unauthenticated server requires the new firmware and pairing too. Do not replace a working old server with the protected backend while its device still runs old firmware.

A Docker daemon proxy bound only to host loopback may prevent bridge-network image builds. Where supported, set `PODCAST_BUILD_NETWORK=host` for the build; the application still runs on its ordinary Compose network. Docker Desktop and server platforms differ. An unavailable build is reported; no successful installation is claimed.

Runtime requests connect directly by default. If your network requires a proxy, set `PODCAST_HTTP_PROXY` and `PODCAST_HTTPS_PROXY` in the private `server/.env` and use an address reachable from an ordinary container. A host loopback proxy address is not reachable by the container. Build proxy configuration and runtime proxy configuration are independent.

On a fresh installation, catalogs arrive before full audio preparation. Public source includes eleven initial shows; each household controls its own subscriptions afterwards. The defaults refresh immediately, then every three hours, and prepare the latest three episodes of each show, newest first. Cache management under Add a show can change prefetch and retention counts; see [Listening and storage](use.md). One hour of device audio is about 115 MB, with additional original and browser audio; leave sufficient disk space for the chosen subscriptions. A completed episode cache is reused.

The public backend requires an administrator browser login and paired device credentials by default. Existing household deployments, personal debugging configuration and real listening records are excluded from the installation package.

The installer records the host user/group and runs the backend with that identity to read/write its private data directory. Reuse the original installation account. A mismatched or invalid saved identity is rejected without overwriting data or changing its owner.

## Hardware: choose the correct operation

The target modern table contains `nvs`, `phy_init`, `factory`, `store`, `netcfg`, and `recovery`; factory starts at `0x10000`, Recovery at `0x6C0000`. App size must be at most 6 MiB. A valid permanent-Recovery program is required for the protected USB upgrade tool. Merely having a matching partition table is insufficient.

- **Already on that modern layout with working Recovery:** install Python 3.10 or later and run `python3 -m pip install -r scripts/flash-requirements.txt` (on Windows use `py -m pip install -r scripts/flash-requirements.txt`), then run `python3 scripts/flash-device.py --manifest release/manifest.json`. On Windows substitute `py` for `python3`. Run from the extracted installation bundle directory. Choose the USB port explicitly. The tool checks the chip, flash capacity, package hashes, table, Recovery image and pending-install journal before confirmation. It writes and verifies only the application. It does not erase settings or rewrite the table, manufacturer identity or Recovery. Opening USB can restart the device.
- **First podcast installation through an existing compatible phone installer:** compatible permanent Recovery must already have been initialized. Upload only `FoloToy-AI-Passport-full.bin` as a merged image from offset zero. The reviewed modern phone installer may erase ordinary application data. Reconfigure and pair afterwards. This is not a progress-preserving upgrade. The phone installer does not install the package's bootloader; boot-button Recovery continues to depend on the device's existing bootloader.
- **Old layout or unknown Recovery:** this project's safe tool refuses installation. It does not initialize permanent Recovery, migrate old identity, or offer erase-all as a shortcut. Use a verified manufacturer/community baseline suitable for the actual device first. The official web flasher supports choosing full or segmented files, but this project has not verified its identity-preservation algorithm. Do not assume that an install preserves a manufacturer's identity.

The application-only `FoloToy-AI-Passport.bin` must never be written at offset zero. The generic full package contains the bootloader, table, and podcast application, but no permanent-Recovery binary. Reserving a Recovery partition does not install Recovery. Do not bypass the checks by writing that image from zero to a device with an unknown layout. This version does not provide a first-initialization path that preserves manufacturer identity on all factory devices. Exact compatibility and references are in [firmware Recovery notes](../firmware/docs/podcast-recovery.md).

## Pair once

Sign in to the browser, open My devices, and generate a code. Only the latest six-digit code is valid, for ten minutes and one device. On first start, the hardware shows a temporary encrypted setup network and its password. Connect a phone, manually open `http://192.168.4.1`, then enter Wi-Fi, the server address and the code. Disable mobile-data fallback if it prevents reaching the temporary network.

The hardware saves configuration only after the real network and pairing succeed. A failed attempt preserves its previous configuration. Reconfiguration is available by holding the confirm button on the playback page. My devices supports renaming and revoking a device. Revocation does not delete listening records.

## Restore the manufacturer's factory identity card

The manufacturer provides a [factory-firmware restoration guide](https://ai-passport.folotoy.cn/guides/restore-factory-firmware/). This is separate from podcast-server pairing and the modern installer's boot-button Recovery:

1. Scan the QR code on the back of **your own device** with your phone and open its individual device page.
2. Transfer the complete link privately to your own computer and open it in desktop Chrome or Edge. It includes the device SN and KEY; do not publish it or send it to others.
3. Connect the powered-on device to the computer using a Type-C cable that supports data transfer.
4. At the bottom of the device page, choose the factory-firmware restoration entry, then connect and verify the device. Choose the port named `USB JTAG/serial debug unit`; verify the device, confirm the manufacturer's instructions, and execute the full restoration.
5. Keep the page and cable connected through download, validation, writing, and automatic restart. Disconnect only after completion, then reconnect in the manufacturer's WeChat mini-program and synchronize the identity-card content.

This is the manufacturer's documented route, not a restoration performed by this project. The individual page's identity checks and data-migration algorithm were not tested. Restoration may clear podcast settings and unsynchronized progress on the hardware. Keeping your own backend data preserves already synchronized listening history; pair again if you reinstall the podcast firmware later.

With the device awake and its USB port recognized, replacing its application interface does not itself remove USB flashing. Espressif's [controller documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/usb-serial-jtag-console.html) confirms that this hardware interface can put the chip into download mode. If no port appears, check power/wake state, a data-capable cable, the port, and other serial programs, then follow manufacturer support guidance. UP is not the chip's download-mode button. Do not erase the whole device as a preliminary recovery step.

## Current validation boundary

The public package needs separate hardware acceptance for first-use backend setup, phone provisioning, pairing, button-to-Recovery behavior and continuous listening. Existing device startup/playback records, host checks and isolated backend tests establish only the paths they actually cover. They do not establish first installation, ear-checked sound quality or Recovery entry. See [Validation](validation.md) for completed and pending checks; universal ready-to-install support has not been established.
