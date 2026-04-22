Import("env")

from pathlib import Path
import platform
import subprocess
import sys


PROJECT_DIR = Path(env["PROJECT_DIR"])


def _repo_dir_from_project(project_dir: Path) -> Path:
    resolved = project_dir.resolve()
    for candidate in (resolved, *resolved.parents):
        if (candidate / "tools" / "windows_updater").exists():
            return candidate
    raise FileNotFoundError(f"Unable to locate tools/windows_updater from {project_dir}")


REPO_DIR = _repo_dir_from_project(PROJECT_DIR)
SCRIPT_DIR = REPO_DIR / "tools" / "windows_updater"
PYTHON = env.subst("$PYTHONEXE") or sys.executable


def _run_step(script_name: str, *args: str) -> None:
    script_path = SCRIPT_DIR / script_name
    print(f"[updater_bundle] running {script_path} {' '.join(args)}".strip())
    subprocess.run([PYTHON, str(script_path), *args], cwd=REPO_DIR, check=True)


def _bundle_args(active_env: str) -> list[str]:
    if active_env.startswith("remote-"):
        return [
            "--output-base",
            "SG98-Remote-Updater",
            "--publish-subdir",
            "remote",
        ]
    return [
        "--output-base",
        "SG98-Updater",
        "--publish-subdir",
        "controller",
    ]


def _host_macos_rid() -> str:
    if platform.system().lower() != "darwin":
        return "osx-arm64"

    machine = platform.machine().lower()
    if machine in ("arm64", "aarch64"):
        return "osx-arm64"
    return "osx-x64"


def bundle_windows_updater(source=None, target=None, env_=None, **kwargs):
    selected_env = env_ or env
    active_env = selected_env.subst("$PIOENV")
    build_dir = Path(selected_env.subst("$BUILD_DIR")).resolve()
    _run_step("fetch_esptool.py")
    _run_step("prepare_assets.py", "--build-dir", str(build_dir))
    _run_step("build_updater.py", "--rid", "win-x64", *_bundle_args(active_env))


def bundle_macos_updater(source=None, target=None, env_=None, **kwargs):
    selected_env = env_ or env
    active_env = selected_env.subst("$PIOENV")
    build_dir = Path(selected_env.subst("$BUILD_DIR")).resolve()
    _run_step("prepare_assets.py", "--build-dir", str(build_dir))
    _run_step("build_updater.py", "--rid", _host_macos_rid(), *_bundle_args(active_env))


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
