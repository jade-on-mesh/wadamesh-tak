# SPDX-License-Identifier: GPL-3.0-or-later
# PlatformIO pre-script for the IDF 5 spike only (platformio_idf5.ini): keep
# compiling after a failed file, like `scons -k`, so one CI run lists every
# compile error instead of the first. `pio run` has no flag for it.
Import("env")  # noqa: F821

try:
    from SCons.Script import Main

    setattr(Main.OptionsParser.values, "keep_going", True)
    print("keep_going: on (every compile error is reported)")
except Exception as e:  # noqa: BLE001 - diagnostics only
    print("keep_going: not available (%s)" % e)
