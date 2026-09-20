"""CI regression guard for the x64 compatibility package, not an OS emulator."""
import pathlib
import sys

import pefile

paths = [pathlib.Path(value) for value in sys.argv[1:]]
files = []
for path in paths:
    files.extend(path.rglob("*.exe") if path.is_dir() else [path])
    if path.is_dir():
        files.extend(path.rglob("*.dll"))
if not files:
    raise SystemExit("No Windows binaries found")
for path in files:
    if path.name.lower().startswith("qt6"):
        raise SystemExit(f"Qt 6 must not enter the x64 compatibility package: {path}")
    pe = pefile.PE(str(path))
    if pe.FILE_HEADER.Machine != 0x8664:
        raise SystemExit(f"Not an x64 binary: {path}")
    # Server 2016 does not export these from Kernel32 in the same way as newer
    # Windows. Dynamic feature detection is OK; unconditional imports are not.
    for library in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for symbol in library.imports:
            if symbol.name in (b"SetThreadDescription", b"GetThreadDescription"):
                raise SystemExit(f"Unsupported unconditional import in {path}: {symbol.name!r}")
    pe.close()
print(f"Checked {len(files)} x64 binaries: no Qt 6 or thread-description imports")
