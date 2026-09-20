#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_root/scripts/regress/xvfb.sh"
source "$repo_root/scripts/regress/scenarios.sh"
app="${1:-$repo_root/build/DuskStudio_artefacts/Release/DuskStudio}"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/dusk-plugin-picker.XXXXXX")"
trap 'xvfb_session_stop; rm -rf "$scratch"' EXIT

for attempt in 1 2 3; do
    run_dir="$scratch/$attempt"
    mkdir -p "$run_dir"
    sandbox_env "$run_dir"
    python3 - "$repo_root" "$run_dir/home/.config/Dusk Studio" <<'PY'
import json
from pathlib import Path
import sys

root, cache_dir = map(Path, sys.argv[1:])
cache_dir.mkdir(parents=True, exist_ok=True)
fixtures = [
    ("clap-cache.json", "CLAP", "Picker Alpha", "Scenario Maker A", "Fx|EQ",
     root / "build-tests/dusk-studio-multi-bus-clap-fixture.clap", "studio.dusk.test.multi-bus"),
    ("lv2-native-cache.json", "LV2", "Picker Beta", "Scenario Maker B", "Fx|Delay",
     root / "build-tests/tests/file-state-fixture.lv2", "urn:duskstudio:test:control-state"),
]
for cache, fmt, name, maker, category, location, plugin_id in fixtures:
    assert location.exists(), location
    descriptor = dict(version=1, backend="native", name=name, manufacturer=maker,
                      category=category, format_name=fmt, location=str(location),
                      plugin_id=plugin_id, num_input_channels=2, num_output_channels=2,
                      is_instrument=False)
    (cache_dir / cache).write_text(json.dumps(dict(version=1, descriptors=[descriptor])))
PY
    xvfb_run 60 env -u DBUS_SESSION_BUS_ADDRESS "${SANDBOX_ENV[@]}" \
        DUSKSTUDIO_RUN_SCENARIOS=gui:gui.plugin_picker_filter_and_load \
        "DUSKSTUDIO_FIXTURE_DIR=$repo_root/build-tests:$repo_root/tests/fixtures" \
        "$app" | tee "$run_dir/output.log"
    grep -Fq '[PASS] gui.plugin_picker_filter_and_load' "$run_dir/output.log"
done
