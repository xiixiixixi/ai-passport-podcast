#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir
    local section_link_flag="-Wl,--gc-sections"
    if [[ "$(uname -s)" == "Darwin" ]]; then section_link_flag="-Wl,-dead_strip"; fi

    python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_podcast_pcm.c main/podcast_pcm.c \
        -o "${test_dir}/test_podcast_pcm"
    "${test_dir}/test_podcast_pcm"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/podcast_wifi_stubs -Imain \
        tests/test_podcast_wifi_events.c main/podcast_wifi_events.c \
        -o "${test_dir}/test_podcast_wifi_events"
    "${test_dir}/test_podcast_wifi_events"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_podcast_position.c main/podcast_position.c \
        -o "${test_dir}/test_podcast_position"
    "${test_dir}/test_podcast_position"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_podcast_ruler.c -o "${test_dir}/test_podcast_ruler"
    "${test_dir}/test_podcast_ruler"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/player_stubs -Imain \
        tests/test_podcast_player_runtime.c main/podcast_position.c main/podcast_pcm.c \
        -o "${test_dir}/test_podcast_player_runtime"
    "${test_dir}/test_podcast_player_runtime"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/podcast_bookmarks_stubs -Imain \
        tests/test_podcast_bookmarks.c -o "${test_dir}/test_podcast_bookmarks"
    "${test_dir}/test_podcast_bookmarks"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/podcast_bookmarks_stubs -Imain \
        tests/test_podcast_sync.c -o "${test_dir}/test_podcast_sync"
    "${test_dir}/test_podcast_sync"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/podcast_bookmarks_stubs -Imain \
        tests/test_podcast_config.c -o "${test_dir}/test_podcast_config"
    "${test_dir}/test_podcast_config"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests -Imain \
        tests/test_podcast_setup_runtime.c -o "${test_dir}/test_podcast_setup_runtime"
    "${test_dir}/test_podcast_setup_runtime"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror tests/test_podcast_recovery_hold.c \
        -o "${test_dir}/test_podcast_recovery_hold"
    "${test_dir}/test_podcast_recovery_hold"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/podcast_controller_stubs -Imain \
        tests/test_podcast_controller.c -o "${test_dir}/test_podcast_controller"
    "${test_dir}/test_podcast_controller"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_podcast_idle.c -o "${test_dir}/test_podcast_idle"
    "${test_dir}/test_podcast_idle"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/podcast_cover_stubs -Imain \
        tests/test_podcast_dynamic_covers.c -o "${test_dir}/test_podcast_dynamic_covers"
    "${test_dir}/test_podcast_dynamic_covers"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" "${section_link_flag}" \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_podcast_product_size.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    python3 tools/check_podcast_product.py "${validation_build_dir}/FoloToy-AI-Passport.bin" "${repo_root}/partitions.csv"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
