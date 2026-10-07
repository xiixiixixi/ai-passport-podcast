[简体中文](README.zh_CN.md) · **English**

# Household backend

Run one server per household. Authorized devices share subscriptions, recent listening and progress. Use the [root installer](../README.md) and [pairing guide](../docs/install.md); no podcast-platform API key is required.

The installer generates a private setup key. First setup consumes it to create an administrator password. Browser sessions require login; device requests require a hashed, individually revocable Bearer token. My devices generates the latest-only, single-use, ten-minute six-digit code. A newly claimed token activates on its first authenticated request, preserving the old token if device storage fails before use. Both active and pending tokens are revoked on device removal.

Device claim: `POST /api/devices/claim` with `code`, persistent `device_id` and optional `name`. Response: the same `device_id` and a 64-character lowercase-hex `token`. All device catalog, progress and audio requests carry `Authorization: Bearer TOKEN`, including HEAD and Range. Devices may write only their own sessions; source and device administration require browser authorization.

`data/` stores catalogs, shared listening and authorization; `media/` stores reusable audio cache. Compose mounts both outside the container. Keep those directories and `.env` private; normal restart/update preserves them. Old unauthenticated hardware requires a coordinated firmware upgrade and pairing before using this protected backend.

To reset only administrator access on your own host: `docker compose --env-file server/.env -f server/compose.yaml exec podcast python manage_access.py reset-admin --confirm`. Rerun the installer to open first setup. Device authorizations and listening records are retained.

Run `python -m unittest discover -s tests -v` here. The suite uses temporary data and includes generated audio, byte ranges, persistent progress, atomic pairing, revocation, ownership and restart restoration. Authentication bypass exists only for explicitly isolated test instances, not the normal service.

See the Chinese companion for additional protocol details. This is a household installation, not a multi-tenant cloud service.
