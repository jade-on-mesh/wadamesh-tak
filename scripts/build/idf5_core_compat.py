# SPDX-License-Identifier: GPL-3.0-or-later
# PlatformIO pre-script for Arduino core 3 / ESP-IDF 5 builds (IDF5_MIGRATION.md).
# Lets the pinned meshcomod core build unmodified until its own IDF 5 fixes land:
#
# - force-includes arch/esp32/idf5_compat.h (esp_mac.h: SerialBLEInterface and others
#   call esp_efuse_mac_get_default, which IDF 5 moved out of esp_system.h);
# - leaves the core's ESP-NOW radio out of the build: its callbacks use IDF 4's
#   signatures, and no Wadamesh board uses ESP-NOW.
#
# Does nothing on platform 6.11 (core 2.0.17, which reports itself as 3.20017.x).
import os

Import("env")  # noqa: F821

_fw = env.PioPlatform().get_package_version("framework-arduinoespressif32") or ""  # noqa: F821
_v = [int(x) for x in _fw.split(".")[:2] if x.isdigit()]
if len(_v) == 2 and _v[0] >= 3 and _v[1] < 10000:
    header = os.path.join(env.subst("$PROJECT_DIR"), "arch", "esp32", "idf5_compat.h")  # noqa: F821
    env.Append(CCFLAGS=["-include", header])  # noqa: F821

    def _skip(env, node):  # noqa: ARG001
        print("idf5_core_compat: leaving out %s (ESP-NOW, unused)" % os.path.basename(str(node)))
        return None

    env.AddBuildMiddleware(_skip, "*ESPNOWRadio.cpp")  # noqa: F821
    print("idf5_core_compat: Arduino core %s" % _fw)
