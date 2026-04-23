Import("env")

from pathlib import Path
import subprocess
import sys


PROJECT_DIR = Path(env["PROJECT_DIR"]).resolve()
PYTHON = env.subst("$PYTHONEXE") or sys.executable
PIOENV = env["PIOENV"]

CONTROLLER_ENVS = ("controller-universal-s3",)
REMOTE_ENVS = ("remote-universal-s3",)


def _repo_root() -> Path:
    if (PROJECT_DIR / "tools").exists() and (PROJECT_DIR / "sg98-remote").exists():
        return PROJECT_DIR
    if PROJECT_DIR.name == "sg98-remote" and (PROJECT_DIR.parent / "tools").exists():
        return PROJECT_DIR.parent
    return PROJECT_DIR


REPO_ROOT = _repo_root()
ROOT_PROJECT_DIR = REPO_ROOT
REMOTE_PROJECT_DIR = PROJECT_DIR if PROJECT_DIR.name == "sg98-remote" else ROOT_PROJECT_DIR


def _run_platformio(project_dir: Path, *args: str) -> None:
    cmd = [PYTHON, "-m", "platformio", *args]
    print(f"[platformio_custom_targets] ({project_dir}) running {' '.join(cmd)}")
    subprocess.run(cmd, cwd=project_dir, check=True)


def _run_env(project_dir: Path, env_name: str, *targets: str) -> None:
    args = ["run", "-d", str(project_dir), "-e", env_name]
    for target_name in targets:
        args.extend(["-t", target_name])
    _run_platformio(project_dir, *args)


def update_packages(source=None, target=None, env_=None, **kwargs):
    _run_platformio(PROJECT_DIR, "pkg", "update", "-d", str(PROJECT_DIR))


def update_clean_build(source=None, target=None, env_=None, **kwargs):
    _run_platformio(PROJECT_DIR, "pkg", "update", "-d", str(PROJECT_DIR))
    _run_env(PROJECT_DIR, PIOENV, "clean")
    _run_env(PROJECT_DIR, PIOENV)


def build_all_controller_hardware(source=None, target=None, env_=None, **kwargs):
    for env_name in CONTROLLER_ENVS:
        _run_env(ROOT_PROJECT_DIR, env_name)


def build_all_remote_hardware(source=None, target=None, env_=None, **kwargs):
    for env_name in REMOTE_ENVS:
        _run_env(REMOTE_PROJECT_DIR, env_name)


def build_all_hardware(source=None, target=None, env_=None, **kwargs):
    build_all_controller_hardware(source=source, target=target, env_=env_, **kwargs)
    build_all_remote_hardware(source=source, target=target, env_=env_, **kwargs)


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
    name="build_all_controller_hardware",
    dependencies=[],
    actions=[build_all_controller_hardware],
    title="Build Universal Controller Firmware",
    description="Build only the shared controller universal firmware image",
)

env.AddCustomTarget(
    name="build_all_remote_hardware",
    dependencies=[],
    actions=[build_all_remote_hardware],
    title="Build Universal Remote Firmware",
    description="Build only the shared remote universal firmware image",
)

env.AddCustomTarget(
    name="build_all_hardware",
    dependencies=[],
    actions=[build_all_hardware],
    title="Build Universal Firmware Set",
    description="Build the controller and remote universal firmware images only",
)
