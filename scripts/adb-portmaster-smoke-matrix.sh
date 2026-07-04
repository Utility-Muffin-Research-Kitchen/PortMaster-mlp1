#!/usr/bin/env bash
set -euo pipefail

python3 - "$@" <<'PY'
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path


TRUE_VALUES = {"1", "true", "yes", "y", "on"}


def env_flag(name):
    return os.environ.get(name, "").strip().lower() in TRUE_VALUES


def sh_quote(value):
    return "'" + str(value).replace("'", "'\"'\"'") + "'"


def run(cmd, *, timeout=None, check=False):
    proc = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=timeout,
    )
    if check and proc.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} failed with {proc.returncode}:\n{proc.stdout}")
    return proc


def choose_adb():
    serial = os.environ.get("ADB_SERIAL", "").strip()
    if serial:
        return ["adb", "-s", serial], serial

    proc = run(["adb", "devices"], check=True)
    devices = []
    for line in proc.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            devices.append(parts[0])
    if not devices:
        raise SystemExit("No online ADB device found; set ADB_SERIAL if needed.")
    return ["adb", "-s", devices[0]], devices[0]


ADB, SERIAL = choose_adb()


def adb_shell(command, *, timeout=None):
    return run(ADB + ["shell", command], timeout=timeout)


def adb_push(src, dst):
    return run(ADB + ["push", str(src), dst], check=True)


def remote_exists(path, kind="e"):
    flags = {"e": "-e", "f": "-f", "d": "-d", "x": "-x"}
    flag = flags[kind]
    return adb_shell(f"[ {flag} {sh_quote(path)} ]").returncode == 0


def remote_grep(path, pattern):
    return adb_shell(f"grep -Eq {sh_quote(pattern)} {sh_quote(path)}").returncode == 0


def command_path(cmd, tools_bin):
    proc = adb_shell(
        f"PATH={sh_quote(tools_bin)}:$PATH command -v {sh_quote(cmd)} 2>/dev/null"
    )
    if proc.returncode != 0:
        return ""
    return proc.stdout.splitlines()[0].strip() if proc.stdout.splitlines() else ""


def first_json_object(text):
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end < start:
        return None
    try:
        return json.loads(text[start : end + 1])
    except json.JSONDecodeError:
        return None


def json_count(doc, *names):
    for name in names:
        value = doc.get(name) if isinstance(doc, dict) else None
        if isinstance(value, int):
            return value
        if isinstance(value, list):
            return len(value)
    return None


def slugify(value):
    value = value.lower().replace(".sh", "")
    value = re.sub(r"[^a-z0-9]+", "-", value)
    return value.strip("-") or "item"


def rel_log_path(name):
    return f"logs/{name}"


platform = os.environ.get("PLATFORM", "mlp1")
ports_filter_raw = os.environ.get("LEAF_PM_SMOKE_PORTS", "")
ports_filter = {
    slugify(part)
    for part in re.split(r"[,:\s]+", ports_filter_raw)
    if part.strip()
}
loop_stress = env_flag("LEAF_PM_SMOKE_LOOP_STRESS")
interactive = env_flag("LEAF_PM_SMOKE_INTERACTIVE")
timeout_s = int(os.environ.get("LEAF_PM_SMOKE_TIMEOUT", "45"))
active_timeout_s = int(os.environ.get("LEAF_PM_SMOKE_ACTIVE_TIMEOUT", str(timeout_s)))
active_hold_s = int(os.environ.get("LEAF_PM_SMOKE_ACTIVE_HOLD", "5"))


def resolve_sdcard():
    explicit = os.environ.get("SDCARD_PATH", "").strip()
    if explicit:
        if not remote_exists(explicit, "d"):
            raise SystemExit(f"SDCARD_PATH does not exist on device: {explicit}")
        return explicit.rstrip("/")

    candidates = os.environ.get("LEAF_PM_SDCARD_CANDIDATES", "/mnt/sdcard /media/sdcard1").split()
    matches = []
    for candidate in candidates:
        candidate = candidate.rstrip("/")
        marker = (
            f"[ -d {sh_quote(candidate)} ] && "
            "{ "
            f"[ -f {sh_quote(candidate + '/.system/leaf/platforms/' + platform + '/enabled')} ] || "
            f"[ -f {sh_quote(candidate + '/.system/leaf/platforms/' + platform + '/launcher/bin/loong_pangu')} ] || "
            f"[ -d {sh_quote(candidate + '/Apps/' + platform + '/PortMaster.pak')} ]; "
            "}"
        )
        if adb_shell(marker).returncode == 0:
            matches.append(candidate)
    matches = list(dict.fromkeys(matches))
    if len(matches) == 1:
        return matches[0]
    if not matches:
        raise SystemExit("Cannot resolve Leaf SD root; set SDCARD_PATH.")
    raise SystemExit(f"Ambiguous Leaf SD roots: {', '.join(matches)}; set SDCARD_PATH.")


sdcard = resolve_sdcard()
pak_dir = f"{sdcard}/Apps/{platform}/PortMaster.pak"
pak_launch = f"{pak_dir}/launch.sh"

if not (remote_exists(pak_dir, "d") and remote_exists(pak_launch, "f")):
    print(f"PortMaster.pak not installed at {pak_dir}; smoke matrix skipped.")
    print("No PortMaster-specific device paths were loaded or written.")
    sys.exit(0)

userdata = f"{sdcard}/.userdata/{platform}"
pm_data = f"{userdata}/portmaster"
pm_tree = f"{pm_data}/PortMaster"
libs_dir = f"{pm_tree}/libs"
tools_bin = f"{pm_data}/compat/tools/aarch64/bin"
report_dir = f"{pm_data}/.leaf/smoke"
remote_logs_dir = f"{report_dir}/logs"
remote_tmp_dir = f"{report_dir}/tmp"

mkdir_proc = adb_shell(
    f"mkdir -p {sh_quote(remote_logs_dir)} {sh_quote(remote_tmp_dir)}"
)
if mkdir_proc.returncode != 0:
    raise SystemExit(f"Unable to create smoke report directory:\n{mkdir_proc.stdout}")

generated_at = datetime.now(timezone.utc).replace(microsecond=0).isoformat()
results = []

tmpdir = Path(tempfile.mkdtemp(prefix="portmaster-smoke-"))
logs_dir = tmpdir / "logs"
logs_dir.mkdir(parents=True, exist_ok=True)


def add(category, subject, mode, status, detail, log=""):
    results.append(
        {
            "category": category,
            "subject": subject,
            "mode": mode,
            "status": status,
            "detail": re.sub(r"\s+", " ", detail).strip(),
            "log": log,
        }
    )


def write_log(name, content):
    safe = slugify(name)
    path = logs_dir / f"{safe}.log"
    path.write_text(content or "", encoding="utf-8", errors="replace")
    return rel_log_path(path.name)


def remote_tail(path, lines=120):
    if not path or not remote_exists(path, "f"):
        return ""
    proc = adb_shell(f"tail -{int(lines)} {sh_quote(path)}")
    return proc.stdout


def run_doctor(name, extra_env=None):
    env = {
        "PLATFORM": platform,
        "SDCARD_PATH": sdcard,
        "USERDATA_PATH": userdata,
    }
    if extra_env:
        env.update(extra_env)
    env_prefix = " ".join(f"{key}={sh_quote(value)}" for key, value in env.items())
    proc = adb_shell(
        f"cd {sh_quote(pak_dir)} && {env_prefix} ./launch.sh --doctor-cfw-json",
        timeout=90,
    )
    log = write_log(name, proc.stdout)
    doc = first_json_object(proc.stdout)
    if proc.returncode != 0:
        add("doctor", name, "adb", "fail", f"doctor exited {proc.returncode}", log)
        return
    if doc is None:
        add("doctor", name, "adb", "warn", "doctor exited 0 but JSON was not parsed", log)
        return
    issues = json_count(doc, "issues", "issue_count", "error_count")
    warnings = json_count(doc, "warnings", "warning_count")
    if issues is None:
        issues = 0
    if warnings is None:
        warnings = 0
    if issues:
        status = "fail"
    elif warnings:
        status = "warn"
    else:
        status = "pass"
    add("doctor", name, "adb", status, f"issues={issues} warnings={warnings}", log)


def run_remote_fixture(name, command, success_detail, missing_detail=None):
    proc = adb_shell(command, timeout=timeout_s)
    log = write_log(name, proc.stdout)
    if proc.returncode == 0:
        add("fixture", name, "synthetic", "pass", success_detail, log)
    else:
        add(
            "fixture",
            name,
            "synthetic",
            "fail",
            missing_detail or f"fixture exited {proc.returncode}",
            log,
        )


def run_env_probe_gui():
    snapshot = f"{pm_data}/.leaf/launch-env-gui.json"
    proc = adb_shell(
        " ".join(
            [
                f"cd {sh_quote(pak_dir)}",
                "&&",
                f"PLATFORM={sh_quote(platform)}",
                f"SDCARD_PATH={sh_quote(sdcard)}",
                f"USERDATA_PATH={sh_quote(userdata)}",
                "LEAF_PM_ENV_PROBE=1",
                "./launch.sh --launch-portmaster",
            ]
        ),
        timeout=90,
    )
    log = write_log("env-probe-gui", proc.stdout)
    if proc.returncode != 0:
        add("env-probe", "gui", "adb", "fail", f"probe exited {proc.returncode}", log)
    elif "binary operator expected" in proc.stdout:
        add("env-probe", "gui", "adb", "warn", "probe completed with shell warning", log)
    elif remote_exists(snapshot, "f"):
        add("env-probe", "gui", "adb", "pass", f"snapshot written: {snapshot}", log)
    else:
        add("env-probe", "gui", "adb", "fail", f"snapshot missing: {snapshot}", log)


def run_env_probe_port():
    script = f"{sdcard}/Roms/PORTS/2048.sh"
    snapshot = f"{pm_data}/.leaf/launch-env-port.json"
    if not remote_exists(script, "f"):
        add("env-probe", "port-2048", "adb", "skipped", f"probe script missing: {script}")
        return

    proc = adb_shell(
        " ".join(
            [
                f"cd {sh_quote(sdcard + '/Roms/PORTS')}",
                "&&",
                f"PLATFORM={sh_quote(platform)}",
                f"SDCARD_PATH={sh_quote(sdcard)}",
                f"USERDATA_PATH={sh_quote(userdata)}",
                "LEAF_PM_ENV_PROBE=1",
                "LEAF_PM_ENV_PROBE_MODE=port",
                "bash ./2048.sh",
            ]
        ),
        timeout=timeout_s,
    )
    log = write_log("env-probe-port-2048", proc.stdout)
    if proc.returncode != 0:
        add("env-probe", "port-2048", "adb", "fail", f"probe exited {proc.returncode}", log)
    elif "binary operator expected" in proc.stdout:
        add("env-probe", "port-2048", "adb", "warn", "probe completed with shell warning", log)
    elif remote_exists(snapshot, "f"):
        add("env-probe", "port-2048", "adb", "pass", f"snapshot written: {snapshot}", log)
    else:
        add("env-probe", "port-2048", "adb", "fail", f"snapshot missing: {snapshot}", log)


def run_env_probe_adb_manual():
    script = f"{sdcard}/Roms/PORTS/2048.sh"
    snapshot = f"{pm_data}/.leaf/launch-env-adb.json"
    if not remote_exists(script, "f"):
        add("env-probe", "adb-2048", "adb", "skipped", f"probe script missing: {script}")
        return

    proc = adb_shell(
        " ".join(
            [
                f"cd {sh_quote(sdcard + '/Roms/PORTS')}",
                "&&",
                "LEAF_PM_ENV_PROBE=1",
                "LEAF_PM_ENV_PROBE_MODE=adb",
                "bash ./2048.sh",
            ]
        ),
        timeout=timeout_s,
    )
    log = write_log("env-probe-adb-2048", proc.stdout)
    if proc.returncode != 0:
        add("env-probe", "adb-2048", "adb", "fail", f"probe exited {proc.returncode}", log)
    elif "binary operator expected" in proc.stdout:
        add("env-probe", "adb-2048", "adb", "warn", "probe completed with shell warning", log)
    elif remote_exists(snapshot, "f"):
        add("env-probe", "adb-2048", "adb", "pass", f"snapshot written: {snapshot}", log)
    else:
        add("env-probe", "adb-2048", "adb", "fail", f"snapshot missing: {snapshot}", log)


def tool_status(tool):
    resolved = command_path(tool, tools_bin)
    if resolved:
        if resolved.startswith(tools_bin.rstrip("/") + "/"):
            add("tool", tool, "presence", "pass", f"{tool} resolves from app-local path: {resolved}")
        else:
            add("tool", tool, "presence", "warn", f"{tool} resolves from stock path, not app-local: {resolved}")
        return True
    add("tool", tool, "presence", "missing", f"{tool} is not available with app-local PATH prepended")
    return False


def runtime_status(name, filename):
    path = f"{libs_dir}/{filename}"
    if remote_exists(path, "f"):
        add("runtime", name, "presence", "pass", f"{filename} present")
        return True
    add("runtime", name, "presence", "missing", f"{filename} missing")
    return False


def item_static_findings(item):
    script_path = f"{sdcard}/{item['script']}"
    missing = []
    marker_missing = []

    if not remote_exists(script_path, "f"):
        missing.append(item["script"])
    for rel in item.get("required", []):
        if not remote_exists(f"{sdcard}/{rel}", "e"):
            missing.append(rel)
    alternative_groups = item.get("any_required", [])
    if alternative_groups:
        if not any(
            all(remote_exists(f"{sdcard}/{rel}", "e") for rel in choices)
            for choices in alternative_groups
        ):
            missing.append(
                "one of: "
                + " | ".join(" + ".join(choices) for choices in alternative_groups)
            )
    for runtime in item.get("runtimes", []):
        if not remote_exists(f"{libs_dir}/{runtime}", "f"):
            missing.append(f"libs/{runtime}")
    if not missing and remote_exists(script_path, "f"):
        for marker in item.get("markers", []):
            if not remote_grep(script_path, marker):
                marker_missing.append(marker)

    return missing, marker_missing


MATRIX = [
    {
        "category": "native-sdl2",
        "subject": "VVVVVV",
        "script": "Roms/PORTS/VVVVVV.sh",
        "required": ["Roms/PORTS/VVVVVV/VVVVVV", "Roms/PORTS/VVVVVV/data.zip"],
        "markers": ["LEAF_PM_PORT_ENV=1", "LEAF_PM_RUNTIME_COMPAT_VVVVVV_SDL2_FULLSCREEN|LEAF_PM_SDL2_FULLSCREEN_ENV"],
        "log": "Roms/PORTS/VVVVVV/log.txt",
    },
    {
        "category": "native-sdl2",
        "subject": "SDLPoP",
        "script": "Roms/PORTS/SDLPoP.sh",
        "required": ["Roms/PORTS/sdlpop/prince.aarch64"],
        "markers": ["LEAF_PM_PORT_ENV=1", "LEAF_PM_SDL2_FULLSCREEN_ENV"],
        "log": "Roms/PORTS/sdlpop/log.txt",
    },
    {
        "category": "native-sdl2",
        "subject": "FreeDink",
        "script": "Roms/PORTS/FreeDink.sh",
        "required": ["Roms/PORTS/freedink/freedink"],
        "markers": ["LEAF_PM_PORT_ENV=1", "LEAF_PM_SDL2_FULLSCREEN_ENV"],
        "log": "Roms/PORTS/freedink/log.txt",
    },
    {
        "category": "native-libretro",
        "subject": "2048",
        "script": "Roms/PORTS/2048.sh",
        "required": ["Roms/PORTS/2048/2048_libretro.so.aarch64"],
        "markers": ["LEAF_PM_PORT_ENV=1", "leaf_pm_run_retroarch"],
        "log": "Roms/PORTS/2048/log.txt",
    },
    {
        "category": "gamemaker-sdl2",
        "subject": "Apotris",
        "script": "Roms/PORTS/Apotris.sh",
        "required": ["Roms/PORTS/apotris/Apotris.aarch64", "Roms/PORTS/apotris/Apotris.armhf"],
        "markers": ["LEAF_PM_PORT_ENV=1", "LEAF_PM_SDL2_FULLSCREEN_ENV"],
        "log": "Roms/PORTS/apotris/log.txt",
    },
    {
        "category": "gamemaker-armhf",
        "subject": "Lineoff",
        "script": "Roms/PORTS/Lineoff.sh",
        "required": ["Roms/PORTS/lineoff/gmloader", "Roms/PORTS/lineoff/.leaf-armhf/gmloader", "Roms/PORTS/lineoff/lineoff.apk"],
        "markers": ["LEAF_PM_PORT_ENV=1", "gmloader", "LEAF_PM_SDL2_FULLSCREEN_ENV"],
        "log": "Roms/PORTS/lineoff/log.txt",
    },
    {
        "category": "gamemaker-armhf",
        "subject": "AM2R",
        "script": "Roms/PORTS/AM2R.sh",
        "required": ["Roms/PORTS/am2r/gmloader", "Roms/PORTS/am2r/.leaf-armhf/gmloader", "Roms/PORTS/am2r/am2r.port"],
        "markers": ["LEAF_PM_PORT_ENV=1", "gmloader", "pm_platform_helper"],
        "log": "Roms/PORTS/am2r/log.txt",
    },
    {
        "category": "gamemaker-gmtoolkit-dotnet",
        "subject": "6 Feet Under",
        "script": "Roms/PORTS/6 Feet Under.sh",
        "required": ["Roms/PORTS/6feetunder/gmloadernext.aarch64", "Roms/PORTS/6feetunder/gmloader.json"],
        "any_required": [
            ["Roms/PORTS/6feetunder/assets/data.win"],
            ["Roms/PORTS/6feetunder/6feetunder.port"],
        ],
        "runtimes": ["gmtoolkit.squashfs", "dotnet-8.0.12.squashfs"],
        "markers": ["LEAF_PM_PORT_ENV=1", "LEAF_PM_SDL2_FULLSCREEN_ENV", "gmloadernext"],
        "log": "Roms/PORTS/6feetunder/log.txt",
        "extra_logs": ["Roms/PORTS/6feetunder/patchlog.txt", "Roms/PORTS/6feetunder/patcherr.txt"],
        "interactive": True,
        "active_timeout": 300,
        "process_regex": "gmloadernext[.]aarch64",
    },
    {
        "category": "box86",
        "subject": "Shovel Knight",
        "script": "Roms/PORTS/Shovel Knight.sh",
        "required": ["Roms/PORTS/shovelknight/box86/box86", "Roms/PORTS/shovelknight/libShovelKnight.so"],
        "any_required": [
            ["Roms/PORTS/shovelknight/gamedata/shovelknight/32/ShovelKnight"],
            ["Roms/PORTS/shovelknight/gamedata/32/ShovelKnight"],
        ],
        "markers": ["LEAF_PM_PORT_ENV=1", "box86"],
        "log": "Roms/PORTS/shovelknight/log.txt",
    },
    {
        "category": "godot-4.3",
        "subject": "Duck Dodge",
        "script": "Roms/PORTS/Duck Dodge.sh",
        "required": ["Roms/PORTS/duckdodge/duckdodge.pck"],
        "runtimes": ["godot_4.3.squashfs", "weston_pkg_0.2.squashfs"],
        "markers": ["LEAF_PM_PORT_ENV=1", "godot_4\\.3", "weston_pkg_0\\.2"],
        "log": "Roms/PORTS/duckdodge/log.txt",
    },
    {
        "category": "godot-4.2-upgrade",
        "subject": "Kobold Kastilla",
        "script": "Roms/PORTS/Kobold Kastilla.sh",
        "required": ["Roms/PORTS/koboldkastilla/gamedata/KoboldKastilla.pck"],
        "runtimes": ["godot_4.3.squashfs", "weston_pkg_0.2.squashfs"],
        "markers": ["LEAF_PM_PORT_ENV=1", "godot_4\\.2\\.2", "LEAF_PM_GODOT_WAYLAND_RUNTIME_UPGRADE|LEAF_PM_GODOT_WAYLAND_RUNTIME"],
        "log": "Roms/PORTS/koboldkastilla/log.txt",
    },
    {
        "category": "godot-3-frt",
        "subject": "Cats on Mars",
        "script": "Roms/PORTS/Cats on Mars.sh",
        "required": ["Roms/PORTS/catsonmars/gamedata/CatsOnMarsWindows_patched.pck"],
        "runtimes": ["frt_3.2.3.squashfs"],
        "markers": ["LEAF_PM_PORT_ENV=1", "LEAF_PM_GODOT_FRT_SDL2", "leaf_pm_run_godot_sdl2_runtime"],
        "log": "Roms/PORTS/catsonmars/log.txt",
        "interactive": True,
        "active_timeout": 90,
        "process_regex": "frt_3[.]2[.]3",
    },
    {
        "category": "love2d",
        "subject": "Wolfenstein 3D",
        "script": "Roms/PORTS/Wolfenstein 3D.sh",
        "required": ["Roms/PORTS/wolf3d/love", "Roms/PORTS/wolf3d/libs/liblove-11.5.so"],
        "markers": ["LEAF_PM_PORT_ENV=1", "LEAF_PM_RUNTIME_COMPAT_LOVE_11_5_LIBS"],
        "log": "Roms/PORTS/wolf3d/log.txt",
    },
    {
        "category": "love2d",
        "subject": "Mr. Rescue",
        "script": "Roms/PORTS/Mr. Rescue.sh",
        "required": ["Roms/PORTS/mrrescue/mrrescue.love"],
        "markers": ["LEAF_PM_PORT_ENV=1", "LEAF_PM_RUNTIME_COMPAT_LOVE_11_5_LIBS"],
        "log": "Roms/PORTS/mrrescue/log.txt",
        "interactive": True,
        "active_timeout": 90,
        "process_regex": "love[.]aarch64|mrrescue[.]love",
    },
    {
        "category": "mono-dotnet",
        "subject": "Celeste",
        "script": "Roms/PORTS/Celeste.sh",
        "required": ["Roms/PORTS/celeste/gamedata/Celeste.exe"],
        "runtimes": ["mono-6.12.0.122-aarch64.squashfs"],
        "markers": ["LEAF_PM_PORT_ENV=1", "mono"],
        "log": "Roms/PORTS/celeste/log.txt",
    },
    {
        "category": "mono-dotnet",
        "subject": "StardewValley",
        "script": "Roms/PORTS/StardewValley.sh",
        "required": ["Roms/PORTS/stardewvalley/SVLoader.exe", "Roms/PORTS/stardewvalley/gamedata/StardewValley.exe"],
        "runtimes": ["mono-6.12.0.122-aarch64.squashfs"],
        "markers": ["LEAF_PM_PORT_ENV=1", "mono"],
        "log": "Roms/PORTS/stardewvalley/log.txt",
    },
]


def matrix_included(item):
    if not ports_filter:
        return True
    candidates = {
        slugify(item["category"]),
        slugify(item["subject"]),
        slugify(Path(item["script"]).name),
    }
    return bool(candidates & ports_filter)


def copy_previous_log(item, row_slug):
    log_rel = item.get("log")
    if not log_rel:
        return ""
    log_path = f"{sdcard}/{log_rel}"
    if not remote_exists(log_path, "f"):
        return ""
    proc = adb_shell(f"tail -80 {sh_quote(log_path)}")
    return write_log(f"{row_slug}-previous", proc.stdout)


def evaluate_matrix_item(item):
    row_slug = slugify(f"{item['category']}-{item['subject']}")
    missing, marker_missing = item_static_findings(item)

    previous_log = copy_previous_log(item, row_slug)
    if missing:
        add(
            item["category"],
            item["subject"],
            "static-readiness",
            "missing",
            "missing: " + ", ".join(missing),
            previous_log,
        )
    elif marker_missing:
        add(
            item["category"],
            item["subject"],
            "static-readiness",
            "needs-review",
            "script markers missing: " + ", ".join(marker_missing),
            previous_log,
        )
    else:
        detail = "script, required files, runtimes, and Leaf markers present; launch not run"
        add(item["category"], item["subject"], "static-readiness", "ready", detail, previous_log)


def run_interactive_item(item):
    row_slug = slugify(f"{item['category']}-{item['subject']}")
    missing, marker_missing = item_static_findings(item)
    previous_log = copy_previous_log(item, row_slug)
    if missing:
        add(
            item["category"],
            item["subject"],
            "interactive-launch",
            "skipped",
            "missing: " + ", ".join(missing),
            previous_log,
        )
        return
    if marker_missing:
        add(
            item["category"],
            item["subject"],
            "interactive-launch",
            "skipped",
            "script markers missing: " + ", ".join(marker_missing),
            previous_log,
        )
        return

    timeout = int(item.get("active_timeout", active_timeout_s))
    hold = int(item.get("active_hold", active_hold_s))
    script_path = f"{sdcard}/{item['script']}"
    port_log = f"{sdcard}/{item.get('log', '')}" if item.get("log") else ""
    extra_logs = [f"{sdcard}/{rel}" for rel in item.get("extra_logs", [])]
    expected_re = item.get("process_regex", "")
    remote_stdout = f"{remote_logs_dir}/{row_slug}-interactive-stdout.log"
    remote_ps = f"{remote_logs_dir}/{row_slug}-interactive-ps.log"
    remote_driver = f"{remote_tmp_dir}/{row_slug}-interactive.sh"

    extra_log_words = " ".join(sh_quote(path) for path in extra_logs)

    driver = f"""#!/bin/sh
set +e
PORT_SCRIPT={sh_quote(script_path)}
PORTS_DIR={sh_quote(sdcard + '/Roms/PORTS')}
REMOTE_STDOUT={sh_quote(remote_stdout)}
REMOTE_PS={sh_quote(remote_ps)}
PORT_LOG={sh_quote(port_log)}
EXPECTED_RE={sh_quote(expected_re)}
FAIL_RE='segmentation fault|trace/breakpoint trap|illegal instruction|bus error|command not found|no such file or directory|permission denied|error while loading shared libraries|cannot execute|Traceback'
TIMEOUT_S={timeout}
HOLD_S={hold}
export PLATFORM={sh_quote(platform)}
export SDCARD_PATH={sh_quote(sdcard)}
export USERDATA_PATH={sh_quote(userdata)}
export LEAF_PM_SMOKE_ACTIVE=1
export LEAF_PM_SMOKE_SUBJECT={sh_quote(item['subject'])}

mkdir -p "$(dirname "$REMOTE_STDOUT")"
rm -f "$REMOTE_STDOUT" "$REMOTE_PS"
cd "$PORTS_DIR" || exit 97

echo "leaf-smoke: starting $PORT_SCRIPT timeout=$TIMEOUT_S hold=$HOLD_S" >"$REMOTE_STDOUT"
setsid sh -c 'exec bash "$1"' leaf-port-smoke "$PORT_SCRIPT" >>"$REMOTE_STDOUT" 2>&1 &
LAUNCH_PID=$!
echo "leaf-smoke: launcher_pid=$LAUNCH_PID" >>"$REMOTE_STDOUT"
echo "LEAF_PM_SMOKE_LAUNCH_PID=$LAUNCH_PID" >>"$REMOTE_STDOUT"

status=12
detail="expected process not observed before timeout"
i=0
while [ "$i" -lt "$TIMEOUT_S" ]; do
  if ! kill -0 "$LAUNCH_PID" 2>/dev/null; then
    wait "$LAUNCH_PID"
    rc=$?
    status=10
    detail="launcher exited before expected process rc=$rc"
    break
  fi

  ps -eo pid,ppid,pgid,stat,args >"$REMOTE_PS" 2>/dev/null || ps >"$REMOTE_PS" 2>/dev/null || true
  if [ -n "$EXPECTED_RE" ] && grep -E "$EXPECTED_RE" "$REMOTE_PS" >/dev/null 2>&1; then
    status=0
    detail="expected process observed"
    break
  fi

  for _leaf_pm_log in "$REMOTE_STDOUT" "$PORT_LOG" {extra_log_words}; do
    [ -n "$_leaf_pm_log" ] || continue
    [ -f "$_leaf_pm_log" ] || continue
    if grep -Eiq "$FAIL_RE" "$_leaf_pm_log"; then
      status=20
      detail="failure pattern observed in $_leaf_pm_log"
      break
    fi
  done
  [ "$status" -eq 20 ] && break
  sleep 1
  i=$((i + 1))
done

if [ "$status" -eq 0 ]; then
  sleep "$HOLD_S"
  for _leaf_pm_log in "$REMOTE_STDOUT" "$PORT_LOG" {extra_log_words}; do
    [ -n "$_leaf_pm_log" ] || continue
    [ -f "$_leaf_pm_log" ] || continue
    if grep -Eiq "$FAIL_RE" "$_leaf_pm_log"; then
      status=21
      detail="failure pattern observed after startup in $_leaf_pm_log"
      break
    fi
  done
fi

echo "LEAF_PM_SMOKE_STATUS=$status" >>"$REMOTE_STDOUT"
echo "LEAF_PM_SMOKE_DETAIL=$detail" >>"$REMOTE_STDOUT"

kill -TERM -"$LAUNCH_PID" 2>/dev/null || kill -TERM "$LAUNCH_PID" 2>/dev/null || true
sleep 2
kill -KILL -"$LAUNCH_PID" 2>/dev/null || kill -KILL "$LAUNCH_PID" 2>/dev/null || true

exit "$status"
"""

    local_driver = tmpdir / f"{row_slug}-interactive.sh"
    local_driver.write_text(driver, encoding="utf-8")
    adb_push(local_driver, remote_driver)
    chmod_proc = adb_shell(f"chmod 755 {sh_quote(remote_driver)}")
    if chmod_proc.returncode != 0:
        log = write_log(f"{row_slug}-interactive", chmod_proc.stdout)
        add(item["category"], item["subject"], "interactive-launch", "fail", "could not chmod remote driver", log)
        return

    proc = adb_shell(f"sh {sh_quote(remote_driver)}", timeout=timeout + hold + 45)
    pieces = [proc.stdout, remote_tail(remote_stdout, 220)]
    if port_log:
        pieces.append(f"\n--- port log: {port_log} ---\n" + remote_tail(port_log, 160))
    for extra in extra_logs:
        pieces.append(f"\n--- extra log: {extra} ---\n" + remote_tail(extra, 160))
    pieces.append(f"\n--- ps: {remote_ps} ---\n" + remote_tail(remote_ps, 120))
    log = write_log(f"{row_slug}-interactive", "\n".join(piece for piece in pieces if piece))

    detail = "interactive launch failed"
    remote_status = None
    launch_pid = ""
    for line in remote_tail(remote_stdout, 80).splitlines():
        if line.startswith("LEAF_PM_SMOKE_DETAIL="):
            detail = line.split("=", 1)[1].strip()
        elif line.startswith("LEAF_PM_SMOKE_STATUS="):
            try:
                remote_status = int(line.split("=", 1)[1].strip())
            except ValueError:
                remote_status = None
        elif line.startswith("LEAF_PM_SMOKE_LAUNCH_PID="):
            launch_pid = line.split("=", 1)[1].strip()
    if launch_pid.isdigit():
        adb_shell(
            f"kill -TERM -{launch_pid} 2>/dev/null || true; "
            "sleep 1; "
            f"kill -KILL -{launch_pid} 2>/dev/null || true"
        )

    effective_rc = remote_status if remote_status is not None else proc.returncode
    if effective_rc == 0:
        add(item["category"], item["subject"], "interactive-launch", "pass", detail, log)
    elif effective_rc == 10:
        add(item["category"], item["subject"], "interactive-launch", "fail", detail, log)
    elif effective_rc in (20, 21):
        add(item["category"], item["subject"], "interactive-launch", "fail", detail, log)
    elif effective_rc == 12:
        add(item["category"], item["subject"], "interactive-launch", "fail", detail, log)
    else:
        add(item["category"], item["subject"], "interactive-launch", "fail", f"{detail}; driver exited {proc.returncode}", log)


run_env_probe_gui()
run_env_probe_port()
run_env_probe_adb_manual()
run_doctor("doctor-cfw")
if loop_stress:
    run_doctor("doctor-loop-stress", {"LEAF_PM_DOCTOR_LOOP_STRESS": "1"})
else:
    add("doctor", "doctor-loop-stress", "adb", "skipped", "set LEAF_PM_SMOKE_LOOP_STRESS=1 to run")

runtime_status("gmtoolkit", "gmtoolkit.squashfs")
runtime_status("godot-4.3", "godot_4.3.squashfs")
runtime_status("godot-4.2.2", "godot_4.2.2.squashfs")
runtime_status("frt-3.2.3", "frt_3.2.3.squashfs")
runtime_status("mono", "mono-6.12.0.122-aarch64.squashfs")
runtime_status("dotnet", "dotnet-8.0.12.squashfs")
runtime_status("weston", "weston_pkg_0.2.squashfs")

xdelta_ok = tool_status("xdelta3")
dos2unix_ok = tool_status("dos2unix")
sevenzip_ok = tool_status("7z")
innoextract_ok = tool_status("innoextract")
systemctl_ok = tool_status("systemctl")
tool_status("strace")

fixture_base = f"{remote_tmp_dir}/tool-fixtures"
adb_shell(f"rm -rf {sh_quote(fixture_base)} && mkdir -p {sh_quote(fixture_base)}")

if xdelta_ok:
    run_remote_fixture(
        "xdelta3-roundtrip",
        " && ".join(
            [
                f"PATH={sh_quote(tools_bin)}:$PATH",
                f"cd {sh_quote(fixture_base)}",
                "printf leaf-base > base.txt",
                "printf leaf-target > target.txt",
                "xdelta3 -e -s base.txt target.txt delta.xd3",
                "xdelta3 -d -s base.txt delta.xd3 out.txt",
                "cmp target.txt out.txt",
            ]
        ),
        "xdelta3 encoded and decoded a userdata-local fixture",
    )
else:
    add("fixture", "xdelta3-roundtrip", "synthetic", "skipped", "xdelta3 missing")

if dos2unix_ok:
    run_remote_fixture(
        "dos2unix-crlf",
        " && ".join(
            [
                f"PATH={sh_quote(tools_bin)}:$PATH",
                f"cd {sh_quote(fixture_base)}",
                "printf 'a\\r\\nb\\r\\n' > crlf.txt",
                "dos2unix crlf.txt >/dev/null 2>&1",
                "! grep -q '\\r' crlf.txt",
            ]
        ),
        "dos2unix converted a userdata-local CRLF fixture",
    )
else:
    add("fixture", "dos2unix-crlf", "synthetic", "skipped", "dos2unix missing")

if sevenzip_ok:
    run_remote_fixture(
        "7z-roundtrip",
        " && ".join(
            [
                f"PATH={sh_quote(tools_bin)}:$PATH",
                f"cd {sh_quote(fixture_base)}",
                "printf archive-smoke > payload.txt",
                "7z a payload.7z payload.txt >/dev/null",
                "mkdir -p extracted",
                "7z x payload.7z -oextracted >/dev/null",
                "cmp payload.txt extracted/payload.txt",
            ]
        ),
        "7z archived and extracted a userdata-local fixture",
    )
else:
    add("fixture", "7z-roundtrip", "synthetic", "skipped", "7z missing")

if innoextract_ok:
    run_remote_fixture(
        "innoextract-version",
        " && ".join(
            [
                f"PATH={sh_quote(tools_bin)}:$PATH",
                "innoextract --version",
            ]
        ),
        "innoextract executes from app-local PATH; installer extraction fixture not bundled",
    )
else:
    add("fixture", "innoextract", "presence", "skipped", "innoextract missing")

if systemctl_ok:
    add("service-restart", "systemctl-shim", "presence", "ready", "systemctl shim present; restart not invoked by passive smoke")
else:
    add("service-restart", "systemctl-shim", "presence", "missing", "systemctl shim missing")

prepare_helper = f"{pak_dir}/scripts/prepare-port-runtime.sh"
if remote_exists(prepare_helper, "f"):
    swapon_proc = adb_shell("cat /proc/swaps 2>/dev/null || true")
    active = "zram" in swapon_proc.stdout
    detail = "prepare helper present; zram currently active" if active else "prepare helper present; zram left inactive by passive smoke"
    add("zram-memory", "prepare-port-runtime", "presence", "ready", detail)
else:
    add("zram-memory", "prepare-port-runtime", "presence", "missing", "prepare helper missing")

for item in MATRIX:
    if matrix_included(item):
        evaluate_matrix_item(item)

interactive_rows = 0
if interactive:
    for item in MATRIX:
        if matrix_included(item) and item.get("interactive"):
            interactive_rows += 1
            run_interactive_item(item)

if interactive and interactive_rows == 0:
    add(
        "interactive",
        "manual-confirmation",
        "manual",
        "manual",
        "LEAF_PM_SMOKE_INTERACTIVE=1 set, but no active launch rows matched LEAF_PM_SMOKE_PORTS",
    )

adb_shell(f"rm -rf {sh_quote(fixture_base)}")

summary = Counter(item["status"] for item in results)
report = {
    "schema": 1,
    "generated_at": generated_at,
    "device_serial": SERIAL,
    "platform": platform,
    "sdcard_path": sdcard,
    "pak_dir": pak_dir,
    "report_dir": report_dir,
    "ports_filter": sorted(ports_filter),
    "interactive": interactive,
    "loop_stress": loop_stress,
    "summary": dict(sorted(summary.items())),
    "results": results,
}

tsv_path = tmpdir / "latest.tsv"
json_path = tmpdir / "latest.json"
with tsv_path.open("w", encoding="utf-8", newline="") as handle:
    writer = csv.DictWriter(
        handle,
        fieldnames=["category", "subject", "mode", "status", "detail", "log"],
        dialect="excel-tab",
    )
    writer.writeheader()
    for item in results:
        writer.writerow(item)
json_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

adb_push(str(logs_dir) + "/.", remote_logs_dir + "/")
adb_push(tsv_path, f"{report_dir}/latest.tsv")
adb_push(json_path, f"{report_dir}/latest.json")
shutil.rmtree(tmpdir)

print(f"PortMaster smoke matrix written to {report_dir}")
print("Summary:")
for key, value in sorted(summary.items()):
    print(f"  {key}: {value}")
print(f"TSV:  {report_dir}/latest.tsv")
print(f"JSON: {report_dir}/latest.json")
PY
