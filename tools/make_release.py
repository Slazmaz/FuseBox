"""Zips build/ into build/FuseBox-<version>-windows-x64.zip (on Linux
-linux-x64.zip, of the folder tools/make_linux.sh gives), for a GitHub
release: FuseBox looks for a file named so in the latest release (update.c).
Only FuseBox's own files go in, never what it keeps next to its exe.

Standard library only: build, then python tools/make_release.py
"""
import glob
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINUX = sys.platform == "linux"
BUILD = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build")
PROGRAMS = ("fusebox", "fusebox-cli", "libfuseboxlan.so") if LINUX else ("fusebox.exe", "fusebox-cli.exe", "*.dll")
LICENSES = {
    "LICENSE": "LICENSE",
    "licenses/rcheevos.txt": "third_party/rcheevos/LICENSE",
}
if not LINUX:
    LICENSES["licenses/MinHook.txt"] = "third_party/minhook/LICENSE.txt"


def read_version(path, pattern):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return re.search(pattern, f.read()).group(1)


version = read_version("CMakeLists.txt", r"project\(fusebox VERSION ([\d.]+)")
built = read_version(os.path.join(BUILD, "generated/version.h"), r'FUSEBOX_VERSION "([\d.]+)"')
if version != built:
    sys.exit(f"build/ is {built}, CMakeLists.txt says {version}: build first")

files = {name: os.path.join(ROOT, path) for name, path in LICENSES.items()}
for pattern in PROGRAMS + ("fonts/*",):
    found = glob.glob(os.path.join(BUILD, pattern))
    if not found:
        sys.exit(f"build/{pattern} isn't there: build first")
    for path in found:
        files[os.path.relpath(path, BUILD).replace(os.sep, "/")] = path

out = os.path.join(BUILD, f"FuseBox-{version}-{'linux' if LINUX else 'windows'}-x64.zip")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name in sorted(files):
        info = zipfile.ZipInfo.from_file(files[name], "FuseBox/" + name)
        info.compress_type = zipfile.ZIP_DEFLATED
        if LINUX:
            info.external_attr = (0o100755 if name in PROGRAMS else 0o100644) << 16
        with open(files[name], "rb") as f:
            z.writestr(info, f.read(), compresslevel=9)
print(out)
print(f'gh release create v{version} "{out}" --title "FuseBox {version}" --generate-notes')
