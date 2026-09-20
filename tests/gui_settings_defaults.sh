#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_root/scripts/regress/xvfb.sh"
source "$repo_root/scripts/regress/scenarios.sh"
app="${1:-$repo_root/build/DuskStudio_artefacts/Release/DuskStudio}"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/dusk-settings-defaults.XXXXXX")"
trap 'xvfb_session_stop; rm -rf "$scratch"' EXIT

for enabled in 0 1; do
    for attempt in 1 2 3; do
        run_dir="$scratch/$enabled-$attempt"
        mkdir -p "$run_dir"
        sandbox_env "$run_dir"
        mkdir -p "$run_dir/config/Dusk Studio"
        printf 'tape_strip_expanded_default=%s\nfollow_playhead_default=%s\n' \
            "$enabled" "$enabled" > "$run_dir/config/Dusk Studio/app-config.properties"
        xvfb_run 60 env -u DBUS_SESSION_BUS_ADDRESS "${SANDBOX_ENV[@]}" \
            DUSKSTUDIO_RUN_SCENARIOS=gui:gui.settings_defaults "$app" | tee "$run_dir/output.log"
        grep -Fq '[PASS] gui.settings_defaults' "$run_dir/output.log"
    done
done
