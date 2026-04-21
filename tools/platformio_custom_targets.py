Import("env")

from pathlib import Path
import subprocess
import sys


PROJECT_DIR = Path(env["PROJECT_DIR"])
PYTHON = env.subst("$PYTHONEXE") or sys.executable
PIOENV = env["PIOENV"]


def _run_platformio(*args):
    cmd = [PYTHON, "-m", "platformio", *args]
    print(f"[platformio_custom_targets] running {' '.join(cmd)}")
    subprocess.run(cmd, cwd=PROJECT_DIR, check=True)


def update_packages(source=None, target=None, env_=None, **kwargs):
    _run_platformio("pkg", "update", "-d", str(PROJECT_DIR))


def update_clean_build(source=None, target=None, env_=None, **kwargs):
    _run_platformio("pkg", "update", "-d", str(PROJECT_DIR))
    _run_platformio("run", "-d", str(PROJECT_DIR), "-e", PIOENV, "-t", "clean")
    _run_platformio("run", "-d", str(PROJECT_DIR), "-e", PIOENV)


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
