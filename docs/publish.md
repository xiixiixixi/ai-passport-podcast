[简体中文](publish.zh_CN.md) · **English**

# Prepare a public release

Run the complete firmware gate with ESP-IDF 5.5.3 and backend tests, then use `python3 scripts/export-release.py --archive VERIFIED_ARCHIVE_DIRECTORY --output dist/public-preview`. Replace the archive placeholder with the actual path printed by this build's successful gate; do not reuse a previous build identity. The exporter checks firmware identities, creates the public upgrade manifest, and excludes runtime/private material. Exporting does not upload or flash the device.

For GitHub, use the exported source package or prepare the repository with the same public-file scope. Exclude `.env`, `.local/`, backend data, cached audio, build/dependency directories, serial and device-identity records, household server configuration, internal repair reports and cleanup backups. Review the manifest and checksums before uploading source, installation instructions and the required firmware files. The public backend requires administrator login and device pairing by default.

The [official Media plays list](https://ai-passport.folotoy.cn/plays/?category=media) contains applications with external backends as well as standalone applications. Their instructions separately cover network access and service credentials; firmware installation alone cannot deploy this server.

Official publication guidance asks for a merged firmware image written from zero, within the device's eight-MiB capacity, a 3:4 cover, Chinese and English names/descriptions/installation steps, and optional public HTTPS source links. Public submission is reviewed. The authenticated publisher form was not tested, so exact private fields and current account permissions remain unverified. The [official web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/) accepts full or segmented files; this does not prove old identity migration or safe upgrades.

Release notes must distinguish first install and app-only upgrade, describe this server dependency and supported layout, and identify hardware checks still pending. If the submission requirements exceed the verified installation scope, identify the release as a preview for compatible devices rather than promising universal installation. Source checks, software-rendered screenshots and playback logs do not establish first installation, hardware photographs or ear-checked audio acceptance. See [Validation](validation.md) for the current evidence.

Suggested name: **随身听** / **Pocket Podcast**. Suggested description: A private household podcast library with a white ruler-based player, Chinese titles, recent listening and shared progress. Users run their own backend and authorize their devices using one-time pairing codes. The backend prepares the latest three episodes per show by default, with browser controls for cache and cleanup. The firmware includes no public hosted audio service, real listening records or another device's credentials.

For a new build, use the exact archive path printed by its successful gate; its hash can differ by machine/build. The release manifest binds the actual firmware files, and the source release must match the build's source. Generated build files may be removed after the matching debug archive and public package are safely retained.
