Import("env")

from pathlib import Path
import platform
import subprocess
import sys


PROJECT_DIR = Path(env["PROJECT_DIR"])
SCRIPT_DIR = PROJECT_DIR / "tools" / "windows_updater"
PYTHON = env.subst("$PYTHONEXE") or sys.executable


def _run_step(script_name: str, *args: str) -> None:
    script_path = SCRIPT_DIR / script_name
    print(f"[updater_bundle] running {script_path} {' '.join(args)}".strip())
    subprocess.run([PYTHON, str(script_path), *args], cwd=PROJECT_DIR, check=True)


def _host_macos_rid() -> str:
    machine = platform.machine().lower()
    if machine in ("arm64", "aarch64"):
        return "osx-arm64"
    return "osx-x64"


def bundle_windows_updater(source=None, target=None, env_=None, **kwargs):
    _run_step("fetch_esptool.py")
    _run_step("prepare_assets.py")
    _run_step("build_updater.py", "--rid", "win-x64")


def bundle_macos_updater(source=None, target=None, env_=None, **kwargs):
    _run_step("prepare_assets.py")
    _run_step("build_updater.py", "--rid", _host_macos_rid())


env.AddCustomTarget(
    name="bundle_windows_updater",
    dependencies=[str(Path("$BUILD_DIR") / "${PROGNAME}.bin")],
    actions=[bundle_windows_updater],
    title="Bundle Windows Updater",
    description="Build the self-contained SG98-Updater.exe package",
)

env.AddCustomTarget(
    name="bundle_macos_updater",
    dependencies=[str(Path("$BUILD_DIR") / "${PROGNAME}.bin")],
    actions=[bundle_macos_updater],
    title="Bundle macOS Updater",
    description="Build the self-contained SG98-Updater package for the current Mac architecture",
)
