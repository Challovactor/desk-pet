#!/usr/bin/env python3
"""board-support/v1 adapter for the pinned ECW6700F B02P12 SDK.

Run from the delivered project root. MOCE_ECW6700_SDK may locate the same SDK
on another machine. This adapter and board sources are included in download;
the registered server recipe runs this exact version from its toolchain mount.
"""
import importlib.util
import os
from pathlib import Path
import sys

SDK = Path(os.environ.get("MOCE_ECW6700_SDK", "/home/moce/moce/tools/firmware/ecw6700f-v1.0.0b02p12/ECW6700FSDK_V1.0.0B02P12/code"))
spec = importlib.util.spec_from_file_location("ecw_vendor_build", SDK / "moce/build.py")
vendor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vendor)
configure_vendor = vendor.configure_reference
run_vendor = vendor.run


def configure(code, source, options):
    board = configure_vendor(code, source, options)
    support = source.parent / "board_support"
    if (support / "board.c").is_file():
        (board / "board_support").symlink_to(support, target_is_directory=True)
        (board / "moce_board_support.c").symlink_to(support / "board.c")
        defs = board / "Make.defs"
        defs.write_text(defs.read_text() + "\nCSRCS += moce_board_support.c\nINCPATHS += $(BOARD_DIR) $(BOARD_DIR)/board_support\n")
    return board


def run(command, **kwargs):
    if str(command[0]) == "/usr/bin/make" and (Path.cwd() / "main/board_support/board.c").is_file():
        # Keep all SDK linker flags; replace only the executable command with
        # the same linker plus the one board-owned pinmux substitution.
        command = list(command) + ["LD=" + str(vendor.TOOLS / (vendor.PREFIX + "ld")) + " --wrap=chip_i2c_pinmux_init"]
    return run_vendor(command, **kwargs)


vendor.configure_reference = configure
vendor.run = run
if __name__ == "__main__":
    try:
        sys.exit(vendor.main())
    except (OSError, ValueError, KeyError, vendor.subprocess.SubprocessError) as error:
        print("MOCE_ECW_BUILD_REJECTED:", error, file=sys.stderr)
        sys.exit(1)
