#!/usr/bin/env python3
from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path
import subprocess


DEFAULT_VERSION = "0.0.0"
APP_ONLY_UPDATE_SINCE = "2026.04.28"
DIRTY_PATHSPEC = [
    ".",
    ":(exclude)tools/windows_updater/publish/**",
]


def repo_root_from(path: Path) -> Path:
    path = path.resolve()
    if path.name == "sg98-remote":
        return path.parent
    if (path / "VERSION").exists():
        return path
    for parent in path.parents:
        if (parent / "VERSION").exists():
            return parent
    return path


def read_release_version(repo: Path) -> str:
    version_path = repo / "VERSION"
    if not version_path.exists():
        return DEFAULT_VERSION
    version = version_path.read_text(encoding="utf-8").strip()
    return version or DEFAULT_VERSION


def _git(repo: Path, *args: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", *args],
            cwd=repo,
            check=True,
            text=True,
            capture_output=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return result.stdout.strip()


def git_short_sha(repo: Path) -> str:
    return _git(repo, "rev-parse", "--short=8", "HEAD") or "nogit"


def git_dirty(repo: Path) -> bool:
    try:
        subprocess.run(["git", "diff", "--quiet", "--", *DIRTY_PATHSPEC], cwd=repo, check=True)
        subprocess.run(["git", "diff", "--cached", "--quiet", "--", *DIRTY_PATHSPEC], cwd=repo, check=True)
        return False
    except (OSError, subprocess.CalledProcessError):
        return True


def role_for_env(env_name: str) -> str:
    return "remote" if env_name.startswith("remote-") else "controller"


def utc_stamp() -> tuple[str, str, str]:
    now = datetime.now(timezone.utc)
    return (
        now.strftime("%Y-%m-%dT%H:%M:%SZ"),
        now.strftime("%Y-%m-%d"),
        now.strftime("%H:%M:%SZ"),
    )


def metadata(repo: Path, env_name: str) -> dict[str, object]:
    generated_at, build_date, build_time = utc_stamp()
    return {
        "schema": "sg98-build-metadata-v1",
        "version": read_release_version(repo),
        "gitSha": git_short_sha(repo),
        "gitDirty": git_dirty(repo),
        "buildEnv": env_name,
        "role": role_for_env(env_name),
        "generatedAtUtc": generated_at,
        "buildDateUtc": build_date,
        "buildTimeUtc": build_time,
    }


def version_slug(version: str, git_sha: str, dirty: bool = False) -> str:
    suffix = "-dirty" if dirty else ""
    return f"v{version}-{git_sha}{suffix}"
