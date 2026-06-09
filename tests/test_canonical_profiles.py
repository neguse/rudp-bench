#!/usr/bin/env python3
import importlib.util
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

EXPECTED = {
    "libs": "raw_udp,mini_rudp,coop_rudp,apex_rudp,enet,kcp,slikenet,raknet,udt4,yojimbo,gns,litenetlib,msquic",
    "media": "1 5 50 75 100 125 150 200",
    "game": "1 5 64 96 128 192 256",
    "echo": "1 50 200 600 1000 1500 2000 3000",
    "reliable_echo": "1 50 200 600 1000 1500 2000 3000",
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

    assert runner.DEFAULT_LIBS == EXPECTED["libs"]
    assert runner.DEFAULT_MEDIA_CONNS == EXPECTED["media"]
    assert runner.DEFAULT_GAME_CONNS == EXPECTED["game"]
    assert runner.DEFAULT_ECHO_CONNS == EXPECTED["echo"]
    assert runner.DEFAULT_RELIABLE_ECHO_CONNS == EXPECTED["reliable_echo"]

    assert renderer.DEFAULT_MEDIA_CONNS == EXPECTED["media"]
    assert renderer.DEFAULT_GAME_CONNS == EXPECTED["game"]
    assert renderer.DEFAULT_ECHO_CONNS == EXPECTED["echo"]
    assert renderer.DEFAULT_RELIABLE_ECHO_CONNS == EXPECTED["reliable_echo"]

    assert shell_var(canonical_sh, "CANONICAL_LIBS") == EXPECTED["libs"]
    assert shell_var(canonical_sh, "CANONICAL_MEDIA_CONNS") == EXPECTED["media"]
    assert shell_var(canonical_sh, "CANONICAL_GAME_CONNS") == EXPECTED["game"]
    assert shell_var(canonical_sh, "CANONICAL_ECHO_CONNS") == EXPECTED["echo"]
    assert shell_var(canonical_sh, "CANONICAL_RELIABLE_ECHO_CONNS") == EXPECTED["reliable_echo"]

    fallback = {row["profile"]: row["conns_schedule"] for row in renderer.DEFAULT_PROFILE_ROWS}
    assert fallback["media_relay"] == EXPECTED["media"]
    assert fallback["game_server"] == EXPECTED["game"]
    assert fallback["reliable_echo"] == EXPECTED["reliable_echo"]
    assert fallback["echo"] == EXPECTED["echo"]

    assert first_conn(EXPECTED["media"]) == 1
    assert first_conn(EXPECTED["game"]) == 1
    assert first_conn(EXPECTED["echo"]) == 1
    assert first_conn(EXPECTED["reliable_echo"]) == 1

    profile = runner.Profile(
        name="media_relay",
        use_case="media_sfu_unreliable_fanout",
        mode="broadcast",
        rate_r=0,
        rate_u=30,
        size=1000,
        conns=[1],
        client_procs=1,
        notes="test",
    )
    capacity = {}
    broken = runner.update_capacity_rows(
        capacity,
        profile,
        ["yojimbo"],
        {
            "yojimbo": {
                "valid": "1",
                "delivery_ratio_median": "0.90",
                "server_cpu_pct_median": "12.3",
            }
        },
        1,
        0.95,
    )
    cap = capacity[("media_relay", "yojimbo")]
    assert broken == ["yojimbo"]
    assert cap["status"] == "below_gate"
    assert cap["last_ok_conns"] == "below_gate"
    assert cap["break_conns"] == "1"
    assert cap["break_delivery"] == "0.90"

    assert renderer.display_last_ok({"status": "below_gate", "last_ok_conns": ""}) == "below_gate"
    assert renderer.display_last_ok({"status": "broken", "last_ok_conns": ""}) == "unmeasured"

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
