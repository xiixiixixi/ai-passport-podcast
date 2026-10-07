<p align="right"><a href="podcast-setup.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Podcast Player Connection and Pairing

The public firmware contains no Wi-Fi credentials or default backend address.
Each household runs its own backend. One backend can authorize several devices;
those devices share the subscribed library and listening progress, while each
keeps its own playback-session identity.

## First connection

1. Install and open your backend, then generate a six-digit pairing code on its
   Devices page. Codes expire after ten minutes and are used once.
2. Start the player. Without saved connection settings it displays a temporary
   `Podcast-xxxx` hotspot, its random 16-character WPA2 password, and
   `http://192.168.4.1`.
3. Connect the phone to that hotspot and manually open the displayed local
   address. The phone need not have Internet access through the hotspot.
4. Enter a 2.4 GHz Wi-Fi network, its password, the root address of your own
   backend, and the pairing code. Use the server's LAN address or a trusted
   HTTPS domain; `localhost` and loopback addresses refer to the player itself.
5. Submit once. The device verifies Wi-Fi and pairs with the backend before
   saving anything. On success it restarts into the existing podcast library
   and closes the hotspot. Reconnect the phone to its ordinary network.

The backend address is at most 160 bytes and has no path, embedded credentials,
query or fragment. Open Wi-Fi may use an empty password; other passwords are
8–63 bytes. Network names are at most 32 bytes, which may be fewer than 32
Chinese characters. HTTPS verifies the normal CA bundle and requires network
clock synchronization; blocked clock service or an untrusted certificate is a
visible setup failure rather than a reason to bypass verification.

## Change or recover a connection

Hold OK on the playback page, then select the last action, **Connection settings**. The
device checkpoints its listening state and restarts into the same temporary
setup page before starting audio. When the library is empty, or pairing is
invalid, holding OK opens the settings action instead of stranding the user.

Generate a new code before pairing again. Wrong Wi-Fi credentials, an invalid
code, an unreachable backend or a failed storage write do not replace the old
settings. With an existing configuration, holding OK on the setup page cancels
and restarts the old player. An ordinary network outage only reconnects; it
never opens a configuration hotspot automatically. Reconfiguration does not
erase bookmarks or the pending listening outbox.

## Identity, storage and requests

Pairing reuses the durable `device-*` client identity in the existing
`podcast_sync` outbox. It does not generate a replacement identity on every
boot, use a MAC address as a secret, or take over another household's backend.
The setup request is `POST /api/devices/claim` with `code` and `device_id`.
Firmware validates the returned identity and a 64-character lowercase-hex
token before committing a versioned, checksummed connection record to the
separate `podcast_cfg` NVS namespace.

Catalogue, preparation, listening, and all audio HEAD/GET requests include the
token in an `Authorization: Bearer` header. Tokens never appear in URLs or
diagnostic output. Authenticated requests do not follow redirects to another
origin. The backend retains an existing token until the saved replacement is
first used, so a failed local save cannot silently revoke the working setup.
Administrators can revoke a device from the backend; 401/403 responses lead to
the connection-settings recovery path rather than automatic credential erasure.

Setup uses one phone, bounded form/response bodies and a temporary HTTP service.
Audio tasks have not started in this boot. The tested Wi-Fi configuration uses
RAM storage; only the validated application record is persisted. Shutdown stops
HTTP callbacks before releasing their queue and lock. A failed server stop
retains those resources until it can be stopped safely or the device restarts.

## Validation boundaries

Host checks execute the real configuration codec, controller, audio request
creation, and setup lifecycle with substituted device/network boundaries.
Native previews render the real setup widgets, verify their glyphs and bounds,
and cycle their lifetime in the 24 KiB LVGL pool. These checks are not a physical
phone/device test. The pending acceptance covers a real phone joining WPA2,
actual DHCP and router access, HTTPS clock/certificate validation, first pairing,
wrong-password retry, restart persistence, cancellation, revocation and audio.

Firmware installation and official recovery compatibility are separate from
network setup. See the [public firmware and Recovery contract](podcast-recovery.md). Use only the verified installation artifacts and partition
instructions delivered with this release; this setup implementation does not
by itself establish official mini-program installation compatibility.
