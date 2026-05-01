Import("env")

from pathlib import Path
import os
import subprocess
import sys


PROJECT_DIR = Path(env["PROJECT_DIR"])
SCRIPT_DIR = PROJECT_DIR / "tools" / "windows_updater"
PYTHON = env.subst("$PYTHONEXE") or sys.executable
DEFAULT_CONTROLLER_ENV = "controller-universal-s3"
DEFAULT_REMOTE_ENV = "remote-universal-s3"
WINDOWS_RID = "win-x64"
MACOS_RID = "osx-arm64"


def _project_core_dir() -> Path:
    try:
        resolved = env.subst("$PROJECT_CORE_DIR")
    except Exception:
        resolved = ""

    if resolved and not resolved.startswith("$"):
        return Path(resolved)
    return Path.home() / ".platformio"


def _toolchain_bin_dirs() -> list[Path]:
    core_dir = _project_core_dir()
    candidates = [
        core_dir / "packages" / "toolchain-xtensa-esp-elf" / "xtensa-esp-elf" / "bin",
        core_dir / "tools" / "toolchain-xtensa-esp-elf" / "xtensa-esp-elf" / "bin",
        core_dir / "packages" / "toolchain-riscv32-esp" / "bin",
        core_dir / "tools" / "toolchain-riscv32-esp" / "bin",
        core_dir / "packages" / "toolchain-xtensa-esp32s3" / "bin",
    ]
    return [candidate for candidate in candidates if candidate.exists()]


def _platformio_subprocess_env() -> dict[str, str]:
    child_env = os.environ.copy()
    existing_path = child_env.get("PATH", "")
    prepend = [str(path) for path in _toolchain_bin_dirs()]
    child_env["PATH"] = os.pathsep.join([*prepend, existing_path] if existing_path else prepend)
    return child_env


def _run_step(script_name: str, *args: str) -> None:
    script_path = SCRIPT_DIR / script_name
    print(f"[updater_bundle] running {script_path} {' '.join(args)}".strip())
    subprocess.run([PYTHON, str(script_path), *args], cwd=PROJECT_DIR, check=True)


def _run_platformio(*args: str) -> None:
    cmd = [PYTHON, "-m", "platformio", *args]
    print(f"[updater_bundle] running {' '.join(cmd)}")
    subprocess.run(cmd, cwd=PROJECT_DIR, check=True, env=_platformio_subprocess_env())


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


def _ota_env(env_name: str, ensure_build: bool) -> None:
    if ensure_build:
        _run_platformio("run", "-d", str(PROJECT_DIR), "-e", env_name)
    _run_step("prepare_assets.py", "--env", env_name)
    _run_step("stage_ota_assets.py", "--env", env_name)


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


def stage_ota_assets(source=None, target=None, env_=None, **kwargs):
    _ota_env((env_ or env)["PIOENV"], ensure_build=False)


def stage_all_ota_assets(source=None, target=None, env_=None, **kwargs):
    _ota_env(DEFAULT_CONTROLLER_ENV, ensure_build=True)
    _ota_env(DEFAULT_REMOTE_ENV, ensure_build=True)


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

env.AddCustomTarget(
    name="stage_ota_assets",
    dependencies=[str(Path("$BUILD_DIR") / "${PROGNAME}.bin")],
    actions=[stage_ota_assets],
    title="Stage OTA Assets",
    description="Stage OTA manifest and application binary for the current firmware environment",
)

env.AddCustomTarget(
    name="stage_all_ota_assets",
    dependencies=[],
    actions=[stage_all_ota_assets],
    title="Stage All OTA Assets",
    description="Build and stage controller and remote OTA assets",
)
