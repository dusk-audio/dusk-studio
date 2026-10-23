#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_root/scripts/regress/xvfb.sh"
source "$repo_root/scripts/regress/scenarios.sh"
app="${1:-$repo_root/build/DuskStudio_artefacts/Release/DuskStudio}"
# Picker Beta is the LV2 fixture where the app hosts LV2 natively and the CLAP
# one where it does not, as the scenario expects. A source build says which in
# its cache; a package has no cache and always ships the LV2 host.
default_beta=LV2
app_cache="$(cd "$(dirname "$app")/../.." 2>/dev/null && pwd)/CMakeCache.txt"
if [[ -f "$app_cache" ]] && ! grep -qx 'DUSKSTUDIO_NATIVE_LV2:BOOL=ON' "$app_cache"; then
    default_beta=CLAP
fi
beta_format="${2:-$default_beta}"
case "$beta_format" in
    LV2|CLAP) ;;
    *) printf 'Usage: %s [app] [LV2|CLAP]\n' "$0" >&2; exit 2 ;;
esac
scratch="$(mktemp -d "${TMPDIR:-/tmp}/dusk-plugin-picker.XXXXXX")"
trap 'xvfb_session_stop; rm -rf "$scratch"' EXIT

for attempt in 1 2 3; do
    run_dir="$scratch/$attempt"
    mkdir -p "$run_dir"
    sandbox_env "$run_dir"
    python3 - "$repo_root" "$run_dir/home/.config/Dusk Studio" "$beta_format" <<'PY'
import json
from pathlib import Path
import sys

root, cache_dir = map(Path, sys.argv[1:3])
cache_dir.mkdir(parents=True, exist_ok=True)
beta = ("lv2-native-cache.json", "LV2", root / "build-tests/tests/file-state-fixture.lv2",
        "urn:duskstudio:test:control-state")
if sys.argv[3] == "CLAP":
    beta = ("clap-cache.json", "CLAP", root / "build-tests/dusk-studio-param-touch-clap-fixture.clap",
            "studio.dusk.test.param-touch")
fixtures = [
    ("clap-cache.json", "CLAP", "Picker Alpha", "Scenario Maker A", "Fx|EQ",
     root / "build-tests/dusk-studio-multi-bus-clap-fixture.clap", "studio.dusk.test.multi-bus"),
    (beta[0], beta[1], "Picker Beta", "Scenario Maker B", "Fx|Delay", beta[2], beta[3]),
    ("clap-cache.json", "CLAP", "Window Pick", "Scenario Maker C", "Fx|Utility",
     root / "build-tests/dusk-studio-no-window-clap-fixture.clap", "studio.dusk.test.no-window"),
]
caches = {}
for cache, fmt, name, maker, category, location, plugin_id in fixtures:
    assert location.exists(), location
    caches.setdefault(cache, []).append(dict(
        version=1, backend="native", name=name, manufacturer=maker, category=category,
        format_name=fmt, location=str(location), plugin_id=plugin_id,
        num_input_channels=2, num_output_channels=2, is_instrument=False))
for cache, descriptors in caches.items():
    (cache_dir / cache).write_text(json.dumps(dict(version=1, descriptors=descriptors)))
PY
    xvfb_run 60 env -u DBUS_SESSION_BUS_ADDRESS "${SANDBOX_ENV[@]}" \
        DUSKSTUDIO_RUN_SCENARIOS=gui:gui.plugin_picker_filter_and_load,gui.pick_opens_native_editor \
        "DUSKSTUDIO_FIXTURE_DIR=$repo_root/build-tests:$repo_root/tests/fixtures" \
        "$app" | tee "$run_dir/output.log"
    grep -Fq '[PASS] gui.plugin_picker_filter_and_load' "$run_dir/output.log"
    grep -Fq '[PASS] gui.pick_opens_native_editor' "$run_dir/output.log"
done
