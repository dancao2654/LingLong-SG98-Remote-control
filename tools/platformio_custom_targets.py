Import("env")

from pathlib import Path
import os
import subprocess
import sys


PROJECT_DIR = Path(env["PROJECT_DIR"])
PYTHON = env.subst("$PYTHONEXE") or sys.executable
PIOENV = env["PIOENV"]


def _repo_dir_from_project(project_dir: Path) -> Path:
    resolved = project_dir.resolve()
    for candidate in (resolved, *resolved.parents):
        if (candidate / "tools" / "windows_updater").exists():
            return candidate
    raise FileNotFoundError(f"Unable to locate repo root from {project_dir}")


REPO_DIR = _repo_dir_from_project(PROJECT_DIR)
REMOTE_PROJECT_DIR = REPO_DIR / "sg98-remote"


def _run_platformio(project_dir: Path, *args):
    cmd = [PYTHON, "-m", "platformio", *args]
    print(f"[platformio_custom_targets] running {' '.join(cmd)}")
    subprocess.run(cmd, cwd=project_dir, check=True)


def _selected_controller_env() -> str:
    override = os.environ.get("SG98_CONTROLLER_BUNDLE_ENV", "").strip()
    if override:
        return override
    if not PIOENV.startswith("remote-"):
        return PIOENV
    return "adafruit_feather_esp32s3"


def _selected_remote_env() -> str:
    override = os.environ.get("SG98_REMOTE_BUNDLE_ENV", "").strip()
    if override:
        return override
    if PIOENV.startswith("remote-"):
        return PIOENV
    return "remote-universal-s3"


def update_packages(source=None, target=None, env_=None, **kwargs):
    _run_platformio(PROJECT_DIR, "pkg", "update", "-d", str(PROJECT_DIR))


def update_clean_build(source=None, target=None, env_=None, **kwargs):
    _run_platformio(PROJECT_DIR, "pkg", "update", "-d", str(PROJECT_DIR))
    _run_platformio(PROJECT_DIR, "run", "-d", str(PROJECT_DIR), "-e", PIOENV, "-t", "clean")
    _run_platformio(PROJECT_DIR, "run", "-d", str(PROJECT_DIR), "-e", PIOENV)


def bundle_all_updaters(source=None, target=None, env_=None, **kwargs):
    controller_env = _selected_controller_env()
    remote_env = _selected_remote_env()

    bundle_steps = [
        (REPO_DIR, controller_env, "bundle_windows_updater"),
        (REPO_DIR, controller_env, "bundle_macos_updater"),
        (REMOTE_PROJECT_DIR, remote_env, "bundle_windows_updater"),
        (REMOTE_PROJECT_DIR, remote_env, "bundle_macos_updater"),
    ]

    print(
        "[platformio_custom_targets] bundling controller "
        f"({controller_env}) and remote ({remote_env}) updater packages"
    )
    for project_dir, env_name, target_name in bundle_steps:
        _run_platformio(project_dir, "run", "-d", str(project_dir), "-e", env_name, "-t", target_name)


env.AddCustomTarget(
    name="update_packages",
    dependencies=[],
    actions=[update_packages],
    title="Update Packages",
    description="Refresh PlatformIO packages and library dependencies",
)

env.AddCustomTarget(
    name="update_clean_build",
    dependencies=[],
    actions=[update_clean_build],
    title="Update, Clean, Build",
    description="Update packages, clean the environment, and rebuild firmware",
)

env.AddCustomTarget(
    name="bundle_all_updaters",
    dependencies=[],
    actions=[bundle_all_updaters],
    title="Bundle All Updaters",
    description="Build controller and remote updater bundles for Windows and macOS in one pass",
)
