Import("env")

from pathlib import Path
import subprocess
import sys


PROJECT_DIR = Path(env["PROJECT_DIR"])
SCRIPT_DIR = PROJECT_DIR / "tools" / "windows_updater"
PYTHON = env.subst("$PYTHONEXE") or sys.executable
DEFAULT_CONTROLLER_ENV = "adafruit_feather_esp32s3"
DEFAULT_REMOTE_ENV = "remote-universal-s3"
WINDOWS_RID = "win-x64"
MACOS_RID = "osx-arm64"


def _run_step(script_name: str, *args: str) -> None:
    script_path = SCRIPT_DIR / script_name
    print(f"[updater_bundle] running {script_path} {' '.join(args)}".strip())
    subprocess.run([PYTHON, str(script_path), *args], cwd=PROJECT_DIR, check=True)


def _run_platformio(*args: str) -> None:
    cmd = [PYTHON, "-m", "platformio", *args]
    print(f"[updater_bundle] running {' '.join(cmd)}")
    subprocess.run(cmd, cwd=PROJECT_DIR, check=True)


def _bundle_config(env_name: str) -> tuple[str, str]:
    if env_name.startswith("remote-"):
        return ("SG98-Remote-Updater", "remote")
    if env_name == "waveshare_esp32_s3_rs485_can":
        return ("SG98-Updater-Waveshare", "controller-waveshare")
    return ("SG98-Updater", "controller")


def _bundle_env(env_name: str, rid: str, ensure_build: bool) -> None:
    output_base, publish_subdir = _bundle_config(env_name)
    if ensure_build:
        _run_platformio("run", "-d", str(PROJECT_DIR), "-e", env_name)
    if rid.startswith("win"):
        _run_step("fetch_esptool.py")
    _run_step("prepare_assets.py", "--env", env_name)
    _run_step(
        "build_updater.py",
        "--rid",
        rid,
        "--output-base",
        output_base,
        "--publish-subdir",
        publish_subdir,
        "--bundle-subdir",
        "bundles",
    )


def bundle_windows_updater(source=None, target=None, env_=None, **kwargs):
    _bundle_env((env_ or env)["PIOENV"], WINDOWS_RID, ensure_build=False)


def bundle_macos_updater(source=None, target=None, env_=None, **kwargs):
    _bundle_env((env_ or env)["PIOENV"], MACOS_RID, ensure_build=False)


def bundle_all_windows_updaters(source=None, target=None, env_=None, **kwargs):
    _bundle_env(DEFAULT_CONTROLLER_ENV, WINDOWS_RID, ensure_build=True)
    _bundle_env(DEFAULT_REMOTE_ENV, WINDOWS_RID, ensure_build=True)


def bundle_all_macos_updaters(source=None, target=None, env_=None, **kwargs):
    _bundle_env(DEFAULT_CONTROLLER_ENV, MACOS_RID, ensure_build=True)
    _bundle_env(DEFAULT_REMOTE_ENV, MACOS_RID, ensure_build=True)


def bundle_all_updaters(source=None, target=None, env_=None, **kwargs):
    bundle_all_windows_updaters(source=source, target=target, env_=env_, **kwargs)
    bundle_all_macos_updaters(source=source, target=target, env_=env_, **kwargs)


env.AddCustomTarget(
    name="bundle_windows_updater",
    dependencies=[str(Path("$BUILD_DIR") / "${PROGNAME}.bin")],
    actions=[bundle_windows_updater],
    title="Bundle Windows Updater",
    description="Build the Windows updater zip for the current firmware environment",
)

env.AddCustomTarget(
    name="bundle_macos_updater",
    dependencies=[str(Path("$BUILD_DIR") / "${PROGNAME}.bin")],
    actions=[bundle_macos_updater],
    title="Bundle macOS ARM64 Updater",
    description="Build the macOS ARM64 updater zip for the current firmware environment",
)

env.AddCustomTarget(
    name="bundle_all_windows_updaters",
    dependencies=[],
    actions=[bundle_all_windows_updaters],
    title="Bundle All Windows Updaters",
    description="Build controller and remote Windows updater zips",
)

env.AddCustomTarget(
    name="bundle_all_macos_updaters",
    dependencies=[],
    actions=[bundle_all_macos_updaters],
    title="Bundle All macOS ARM64 Updaters",
    description="Build controller and remote macOS ARM64 updater zips",
)

env.AddCustomTarget(
    name="bundle_all_updaters",
    dependencies=[],
    actions=[bundle_all_updaters],
    title="Bundle All Updaters",
    description="Build controller and remote updater zips for win-x64 and osx-arm64",
)
