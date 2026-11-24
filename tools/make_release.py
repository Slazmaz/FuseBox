"""Zips build/ into build/FuseBox-<version>-windows-x64.zip, for a GitHub
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
BUILD = os.path.join(ROOT, "build")
LICENSES = {
    "LICENSE": "LICENSE",
    "licenses/rcheevos.txt": "third_party/rcheevos/LICENSE",
    "licenses/MinHook.txt": "third_party/minhook/LICENSE.txt",
}


def read_version(path, pattern):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return re.search(pattern, f.read()).group(1)


version = read_version("CMakeLists.txt", r"project\(fusebox VERSION ([\d.]+)")
built = read_version("build/generated/version.h", r'FUSEBOX_VERSION "([\d.]+)"')
if version != built:
    sys.exit(f"build/ is {built}, CMakeLists.txt says {version}: build first")

files = {name: os.path.join(ROOT, path) for name, path in LICENSES.items()}
for pattern in ("fusebox.exe", "fusebox-cli.exe", "*.dll", "fonts/*"):
    found = glob.glob(os.path.join(BUILD, pattern))
    if not found:
        sys.exit(f"build/{pattern} isn't there: build first")
    for path in found:
        files[os.path.relpath(path, BUILD).replace(os.sep, "/")] = path

out = os.path.join(BUILD, f"FuseBox-{version}-windows-x64.zip")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for name in sorted(files):
        z.write(files[name], "FuseBox/" + name)
print(out)
print(f'gh release create v{version} "{out}" --title "FuseBox {version}" --generate-notes')
