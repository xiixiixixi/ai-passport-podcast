<p align="right"><a href="podcast-recovery.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Public Firmware and Permanent Recovery

The public package targets the modern installer protocol-4 layout verified against
[installer source commit 127c56bc97b7ea85bf4bfd2b52ce4e2d80a30566](https://github.com/SHLcy/ai-passport-miniapp-installer/tree/127c56bc97b7ea85bf4bfd2b52ce4e2d80a30566),
including its exact partition table and bootloader hook. Its behavior differs
from the older 3 MiB application layout and five-second boot-button convention,
and from this project's earlier private test device's minimal layout.

| Label | Type/subtype | Offset | Size |
| --- | --- | ---: | ---: |
| nvs | data/nvs | 0x9000 | 0x6000 |
| phy_init | data/phy | 0xF000 | 0x1000 |
| factory | app/factory | 0x10000 | 0x650000 |
| store | data/nvs | 0x660000 | 0x4000 |
| netcfg | data/nvs | 0x6BC000 | 0x4000 |
| recovery | app/test | 0x6C0000 | 0x140000 |

The installed application has an additional public-release limit of **6 MiB
(0x600000 bytes)**, even though its factory partition is larger. The build gate
rejects larger or missing images before archiving a release. Lossless font
storage preserves existing glyph coverage and four-bit pixel values; shrinking
the image does not itself prove physical installer compatibility.

## USB flashing and permanent Recovery are separate

The manufacturer's [play page](https://ai-passport.folotoy.cn/plays/65/) explains
that installing a play replaces the current one. Its desktop website writes over
USB without assistance from the identity-card or podcast interface. The
ESP32-C3 ROM download entry is separate from the rewritable application,
bootloader, and permanent-Recovery image. Espressif's [controller documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/usb-serial-jtag-console.html)
confirms that native USB can enter download mode automatically. An application
that disables the controller, repurposes its pins, or enters deep sleep can make
the port unavailable and require separate intervention. This project does not
change chip download permissions.

The earlier private test device was checked with three partitions: `nvs` at
`0x9000` with size `0x6000`, `phy_init` at `0xF000` with size `0x1000`, and
`factory` at `0x10000` with size `0x7F0000`. Recent physical updates wrote only
the application, keeping the device's existing bootloader and table. It has no
modern permanent-Recovery partition; UP or combined-button entry into official
BLE Recovery must not be promised. This is also distinct from the older 3 MiB
layout containing a `cardid` partition at `0x356000`.

The generic merged package includes this project's bootloader, modern table,
and application, but no Recovery image at `0x6C0000`. Writing the whole image
from zero does not supply that missing program. The protected app-only tool
does not install the new boot hook, and the phone installer skips the package's
bootloader. Button-to-Recovery behavior therefore depends on the device's
existing bootloader and valid Recovery image; a partition label is not proof.

## Recovery entry

Power on while holding UP. The bootloader samples the externally pulled-up
GPIO0 button node every 10 ms, using the modern 200 ms press and 50 ms release
debounce. With no held key it continues normal boot after 1200 ms. This is a
boot-time path; playback volume and navigation keys keep their existing actions.
The hook also recognizes the install marker at 0x6B8000 and a missing main
application header. It loads the permanent image at 0x6C0000 using the verified
ESP-IDF bootloader API, not an application OTA call.

Permanent Recovery must already have been initialized with the supported
installer. This podcast package does not contain or overwrite that image. If
the region is blank, the hook reports that USB initialization is needed and
continues the ordinary application instead of jumping into blank memory.
Firmware compilation does not establish that Recovery is present on a board.
The reference hook reads the digital level of the shared three-key resistor
node; actual physical button selectivity remains a hardware acceptance check.

## Initial install versus update

The modern phone installer's merged-image path erases ordinary `nvs` and
`store` partitions before copying package data; only `netcfg` and permanent
Recovery are skipped. It does not automatically migrate a board's old identity
bytes at 0x356000. The initial installer USB bundle likewise contains erased
padding at that old identity address. Therefore a merged upload or ordinary
raw write from 0x0 must not be described as preserving a previous device's
identity, bookmarks, unacknowledged listening records or saved pairing.

The delivered protected USB tool supports application updates only on an
already compatible modern layout with valid permanent Recovery. Legacy identity
migration and first initialization are not provided; the tool refuses those
operations. All checks must finish before writing. An
app-only update can preserve configuration and listening records when the
existing partition table is already compatible and the target range is safe;
merely excluding 0x9000 does not protect a legacy identity region inside the
new application's range. Do not perform a full-chip erase as routine setup.

On a fresh device, [connection setup and pairing](podcast-setup.md) supplies its
network, backend address and per-device credential. On a merged reinstall that
resets ordinary NVS, generate a new code and pair again. Web-side listening
history remains in the household backend, while unsynchronized local records
need to be preserved before the installer erases them.

## Restore the manufacturer's factory firmware

Follow the manufacturer's [factory-restoration guide](https://ai-passport.folotoy.cn/guides/restore-factory-firmware/):
scan the QR code on your own device, privately transfer its complete SN/KEY
link to your own computer, and open it in desktop Chrome or Edge. With the
device powered on and connected by a data cable, open the restoration entry
at the bottom of its individual page, connect and verify the correct USB
device, then confirm and execute the full restoration. Keep the cable and page
connected through validation, writing, and automatic restart. Afterwards,
reconnect in the manufacturer's WeChat mini-program to synchronize the identity
card. Detailed steps are in [installation instructions](../../docs/install.md#restore-the-manufacturers-factory-identity-card).

The individual manufacturer's restoration link is separate from this project's
server-pairing code and does not require BLE Recovery inside the podcast
application. Never include that link or its key in a public package. This
project did not enter an individual restoration page, execute browser flashing,
or verify that page's identity-migration algorithm. Restoration is not a
promise to retain device-local podcast settings, bookmarks, or unsynchronized
records. Being able to rewrite firmware is not automatic rollback.

## Verified and pending

The source partition/boot contract, size guard, and debounce state machine have
host checks. Actual public artifacts must additionally pass the complete gate
and matching-image verification. Phone download, permanent-Recovery entry,
legacy identity migration, restart behavior and the complete phone-to-device
installation/pairing flow require separate physical acceptance. No source-level
check or generated preview substitutes for those results.
