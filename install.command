#!/bin/bash
set -e
passport_install_root="$(cd -- "$(dirname -- "$0")" && pwd)"
exec "$passport_install_root/scripts/install-server.sh"
