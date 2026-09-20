"""Check every bundled Mach-O slice, not just the application's deployment target."""
import pathlib
import re
import subprocess
import sys

root, architecture, minimum = sys.argv[1:]
baseline = tuple(map(int, minimum.split(".")))
baseline += (0,) * (3 - len(baseline))
count = 0
for path in pathlib.Path(root).rglob("*"):
    if not path.is_file() or path.is_symlink():
        continue
    kind = subprocess.check_output(["file", "-b", str(path)], text=True)
    if "Mach-O" not in kind:
        continue
    architectures = subprocess.check_output(["lipo", "-archs", str(path)], text=True).split()
    if architecture not in architectures:
        raise SystemExit(f"Missing {architecture}: {path}")
    if len(architectures) > 1:
        # Each release is architecture-specific; don't ship the other CPU slice.
        subprocess.run(["lipo", str(path), "-thin", architecture, "-output", str(path)], check=True)
    commands = subprocess.check_output(["otool", "-arch", architecture, "-l", str(path)], text=True)
    versions = re.findall(r"cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version ([\d.]+)", commands)
    versions += re.findall(r"\bminos ([\d.]+)", commands)
    if not versions:
        raise SystemExit(f"Missing deployment target: {path}")
    for version in versions:
        parts = tuple(map(int, version.split(".")))
        parts += (0,) * (3 - len(parts))
        if parts > baseline:
            raise SystemExit(f"{path} requires macOS {version}, exceeds {minimum}")
    count += 1
if not count:
    raise SystemExit("No Mach-O files found")
print(f"Checked {count} binaries for {architecture}, macOS {minimum}")
