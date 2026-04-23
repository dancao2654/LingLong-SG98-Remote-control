Import("env")


def _add_no_progress_for_esptool():
    protocol = str(env.get("UPLOAD_PROTOCOL", ""))
    if protocol and protocol != "esptool":
        return

    flags = list(env.get("UPLOADERFLAGS", []))
    if "--no-progress" in flags:
        return

    try:
        write_index = flags.index("write-flash")
    except ValueError:
        return

    flags.insert(write_index + 1, "--no-progress")
    env.Replace(UPLOADERFLAGS=flags)


_add_no_progress_for_esptool()
