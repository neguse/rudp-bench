#!/usr/bin/env python3
import importlib.util
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

EXPECTED = {
    "media": "5 50 75 100 125 150 200",
    "game": "5 64 96 128 192 256",
    "echo": "50 200 600 1000 1500 2000 3000",
}


def load_script(path: Path):
    spec = importlib.util.spec_from_file_location(path.stem, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[path.stem] = module
    spec.loader.exec_module(module)
    return module


def shell_var(text: str, name: str) -> str:
    match = re.search(rf'^{name}="([^"]+)"$', text, re.MULTILINE)
    assert match, name
    return match.group(1)


def first_conn(schedule: str) -> int:
    return int(schedule.split()[0])


def main() -> int:
    runner = load_script(ROOT / "scripts" / "run_final_saturation_profiles.py")
    renderer = load_script(ROOT / "scripts" / "render_canonical_report.py")
    canonical_sh = (ROOT / "scripts" / "run_canonical_tests.sh").read_text()

    assert runner.DEFAULT_MEDIA_CONNS == EXPECTED["media"]
    assert runner.DEFAULT_GAME_CONNS == EXPECTED["game"]
    assert runner.DEFAULT_ECHO_CONNS == EXPECTED["echo"]

    assert renderer.DEFAULT_MEDIA_CONNS == EXPECTED["media"]
    assert renderer.DEFAULT_GAME_CONNS == EXPECTED["game"]
    assert renderer.DEFAULT_ECHO_CONNS == EXPECTED["echo"]

    assert shell_var(canonical_sh, "CANONICAL_MEDIA_CONNS") == EXPECTED["media"]
    assert shell_var(canonical_sh, "CANONICAL_GAME_CONNS") == EXPECTED["game"]
    assert shell_var(canonical_sh, "CANONICAL_ECHO_CONNS") == EXPECTED["echo"]

    fallback = {row["profile"]: row["conns_schedule"] for row in renderer.DEFAULT_PROFILE_ROWS}
    assert fallback["media_relay"] == EXPECTED["media"]
    assert fallback["game_server"] == EXPECTED["game"]
    assert fallback["echo"] == EXPECTED["echo"]

    assert first_conn(EXPECTED["media"]) == 5
    assert first_conn(EXPECTED["game"]) == 5
    assert first_conn(EXPECTED["echo"]) == 50

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
