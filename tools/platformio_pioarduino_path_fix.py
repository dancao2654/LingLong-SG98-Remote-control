import os
from pathlib import Path

Import("env")


def _package_names_for_prefix(prefix):
    package_names = ["toolchain-xtensa-esp-elf"]
    if prefix == "xtensa-esp32s3-elf-":
        package_names.append("toolchain-xtensa-esp32s3")
    elif prefix == "xtensa-esp32s2-elf-":
        package_names.append("toolchain-xtensa-esp32s2")
    elif prefix == "xtensa-esp32-elf-":
        package_names.append("toolchain-xtensa-esp32")
    return tuple(package_names)


def _candidate_tool_bins(prefix):
    platform = env.PioPlatform()
    home = Path.home()
    package_names = _package_names_for_prefix(prefix)
    tools_base = home / ".platformio" / "tools" / "toolchain-xtensa-esp-elf"

    yield tools_base / "bin"

    for package_name in package_names:
        try:
            package_dir = platform.get_package_dir(package_name)
        except KeyError:
            package_dir = None
        if not package_dir:
            continue

        base = Path(package_dir)
        yield base / "bin"

    packages_base = home / ".platformio" / "packages"
    for package_name in package_names:
        yield packages_base / package_name / "bin"


def _toolchain_ready(candidate):
    candidate = Path(candidate)
    tool_root = candidate.parent
    possible_sysroots = (
        tool_root / "xtensa-esp-elf",
        tool_root.parent / "xtensa-esp-elf",
    )
    return any((sysroot / "lib").exists() and (sysroot / "include").exists() for sysroot in possible_sysroots)


def _tool_prefix(current):
    for prefix in (
        "xtensa-esp32s3-elf-",
        "xtensa-esp32s2-elf-",
        "xtensa-esp32-elf-",
        "xtensa-esp-elf-",
    ):
        if prefix in current:
            return prefix
    return None


def _board_tool_prefix():
    try:
        mcu = str(env.BoardConfig().get("build.mcu", ""))
    except Exception:
        return None

    return {
        "esp32s3": "xtensa-esp32s3-elf-",
        "esp32s2": "xtensa-esp32s2-elf-",
        "esp32": "xtensa-esp32-elf-",
    }.get(mcu)


def _all_candidate_tool_bins():
    ordered = []
    seen = set()
    for prefix in (
        "xtensa-esp32s3-elf-",
        "xtensa-esp32s2-elf-",
        "xtensa-esp32-elf-",
        "xtensa-esp-elf-",
    ):
        for candidate in _candidate_tool_bins(prefix):
            candidate = Path(candidate)
            if candidate in seen or not candidate.exists():
                continue
            seen.add(candidate)
            ordered.append(candidate)

    return ordered


for candidate in reversed(_all_candidate_tool_bins()):
    env.PrependENVPath("PATH", str(candidate))


current_cxx = str(env.get("CXX", ""))
prefix = _tool_prefix(current_cxx) or _board_tool_prefix()
if prefix:
    exe_suffix = ".exe" if os.name == "nt" else ""
    for candidate in _candidate_tool_bins(prefix):
        cxx_path = candidate / f"{prefix}g++{exe_suffix}"
        if not cxx_path.exists() or not _toolchain_ready(candidate):
            continue

        env.PrependENVPath("PATH", str(candidate))
        replacements = {
            "AS": candidate / f"{prefix}as{exe_suffix}",
            "AR": candidate / f"{prefix}gcc-ar{exe_suffix}",
            "CC": candidate / f"{prefix}gcc{exe_suffix}",
            "CXX": cxx_path,
            "LINK": cxx_path,
            "NM": candidate / f"{prefix}gcc-nm{exe_suffix}",
            "RANLIB": candidate / f"{prefix}gcc-ranlib{exe_suffix}",
            "READELF": candidate / f"{prefix}readelf{exe_suffix}",
            "SIZETOOL": candidate / f"{prefix}size{exe_suffix}",
            "STRIP": candidate / f"{prefix}strip{exe_suffix}",
        }

        for key, value in replacements.items():
            if value.exists():
                env.Replace(**{key: str(value)})
        break
