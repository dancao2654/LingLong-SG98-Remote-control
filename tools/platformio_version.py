Import("env")

from pathlib import Path
import sys


PROJECT_DIR = Path(env["PROJECT_DIR"])
REPO_ROOT = PROJECT_DIR.parent if PROJECT_DIR.name == "sg98-remote" else PROJECT_DIR
TOOLS_DIR = REPO_ROOT / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from sg98_version import metadata  # noqa: E402


def macro_string(value: object) -> str:
    text = str(value)
    if hasattr(env, "StringifyMacro"):
        return env.StringifyMacro(text)
    escaped = text.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


env_name = env["PIOENV"]
info = metadata(REPO_ROOT, env_name)

env.Append(
    CPPDEFINES=[
        ("SG98_RELEASE_VERSION", macro_string(info["version"])),
        ("SG98_FIRMWARE_ROLE", macro_string(info["role"])),
        ("SG98_BUILD_ENV", macro_string(info["buildEnv"])),
        ("SG98_GIT_SHA", macro_string(info["gitSha"])),
        ("SG98_GIT_DIRTY", 1 if info["gitDirty"] else 0),
        ("SG98_BUILD_DATE", macro_string(info["buildDateUtc"])),
        ("SG98_BUILD_TIME", macro_string(info["buildTimeUtc"])),
    ]
)

print(
    "[sg98_version] "
    f"{info['role']} {info['version']} {info['gitSha']}"
    f"{'-dirty' if info['gitDirty'] else ''} env={info['buildEnv']}"
)
