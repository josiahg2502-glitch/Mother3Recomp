"""Stage a player package without ROMs, BIOS dumps, sources or personal settings."""
import argparse
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
APP_BUNDLE = "Mother3Recomp.app"
ICON_SIZES = (16, 32, 128, 256, 512)

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("version")
p.add_argument("platform", choices=["windows-x64", "linux-x86_64", "macos-arm64", "macos-x86_64"])
p.add_argument("binary", type=Path)
p.add_argument("output", type=Path)
p.add_argument("--lib-dir", type=Path)
a = p.parse_args()
if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", a.version):
    p.error("Use a numeric release version")
name = f"Mother3Recomp-v{a.version}-{a.platform}"
stage = a.output.resolve() / name
stage.mkdir(parents=True, exist_ok=False)
windows = a.platform == "windows-x64"
macos = a.platform.startswith("macos-")


def macho_links(binary):
    listing = subprocess.check_output(["otool", "-L", str(binary)], text=True)
    return [line.split(" (compatibility")[0].strip() for line in listing.splitlines()[1:]]


def bundle_dylibs(binary, frameworks):
    pending = [binary]
    while pending:
        image = pending.pop()
        for dependency in macho_links(image):
            if dependency.startswith(("@executable_path", "@loader_path", "/usr/lib", "/System")):
                continue
            if dependency.startswith("@rpath"):
                continue
            target = frameworks / Path(dependency).name
            if not target.exists():
                shutil.copy2(dependency, target)
                subprocess.run(["install_name_tool", "-id", f"@rpath/{target.name}", str(target)], check=True)
                pending.append(target)
            subprocess.run(["install_name_tool", "-change", dependency, f"@rpath/{target.name}", str(image)], check=True)
        subprocess.run(["install_name_tool", "-add_rpath", "@executable_path/../Frameworks", str(image)],
                       check=False, capture_output=True)


def verify_links(executable, frameworks):
    missing = set()
    for image in [executable, *frameworks.iterdir()]:
        for dependency in macho_links(image):
            if dependency.startswith("@rpath") and not (frameworks / Path(dependency).name).exists():
                missing.add(dependency)
    if missing:
        raise SystemExit(f"Unbundled dylib dependencies: {sorted(missing)}")


def make_icns(source, destination):
    iconset = destination.parent / "Mother3.iconset"
    iconset.mkdir()
    png = destination.parent / "Mother3.png"
    subprocess.run(["sips", "-s", "format", "png", str(source), "--out", str(png)], check=True, capture_output=True)
    source_px = int(subprocess.check_output(["sips", "-g", "pixelWidth", str(png)], text=True).rsplit(":", 1)[1])
    for size in ICON_SIZES:
        for scale, suffix in ((1, ""), (2, "@2x")):
            pixels = size * scale
            if pixels > source_px:
                continue
            subprocess.run(["sips", "-z", str(pixels), str(pixels), str(png),
                            "--out", str(iconset / f"icon_{size}x{size}{suffix}.png")],
                           check=True, capture_output=True)
    subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(destination)], check=True)
    shutil.rmtree(iconset)
    png.unlink()


app = stage / APP_BUNDLE
binary_dir = app / "Contents/MacOS" if macos else stage
data_dir = app / "Contents/Resources" if macos else stage
if macos:
    binary_dir.mkdir(parents=True)
    data_dir.mkdir(parents=True)
    (app / "Contents/Frameworks").mkdir()
    info = {
        "CFBundleExecutable": "Mother3Recomp",
        "CFBundleIdentifier": "com.recomp.mother3",
        "CFBundleName": "MOTHER 3",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": a.version,
        "CFBundleVersion": a.version,
        "CFBundleIconFile": "Mother3",
        "LSMinimumSystemVersion": "11.0",
        "NSHighResolutionCapable": True,
    }
    (app / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
shutil.copy2(a.binary, binary_dir / ("Mother3Recomp.exe" if windows else "Mother3Recomp"))
if windows:
    if not a.lib_dir:
        p.error("Windows packaging needs --lib-dir for runtime libraries")
    shutil.copy2(a.binary.parent / "mother3_bezel.dll", stage)
    pending = list(stage.glob("*.exe")) + list(stage.glob("*.dll"))
    seen = set()
    while pending:
        binary = pending.pop()
        if binary.name.lower() in seen:
            continue
        seen.add(binary.name.lower())
        imports = subprocess.check_output(["objdump", "-p", str(binary)], text=True)
        for dll in re.findall(r"DLL Name:\s*(\S+)", imports):
            source = a.lib_dir / dll
            if source.is_file() and not (stage / dll).exists():
                shutil.copy2(source, stage / dll)
                pending.append(stage / dll)
else:
    (binary_dir / "Mother3Recomp").chmod(0o755)
    if not macos:
        launcher = stage / "run_mother3.sh"
        launcher.write_text('#!/bin/sh\ncd -- "$(dirname -- "$0")" || exit 1\nexec ./Mother3Recomp "$@"\n')
        launcher.chmod(0o755)
shutil.copytree(a.binary.parent / "assets/fonts", data_dir / "assets/fonts")
images = data_dir / "assets/img"
images.mkdir(parents=True)
for image in (ROOT / "assets/img").iterdir():
    if image.suffix == ".tga" or image.name in ("flags.png", "icon.png"):
        shutil.copy2(image, images / image.name)
config = (ROOT / "game.toml").read_text(encoding="utf-8")
for section in ("bios", "rom"):
    config = re.sub(r'(\[' + section + r'\]\s*\n)path = "[^"\n]*"\n', r'\1', config)
(data_dir / "game.toml").write_text(config, encoding="utf-8")
shutil.copy2(ROOT / "docs/PLAYER-README.md", stage / "README.md")
shutil.copy2(ROOT / "CREDITS.md", stage)
shutil.copy2(ROOT / "LICENSE", stage)
(stage / "version.txt").write_text(a.version + "\n")
licenses = stage / "Licenses"
licenses.mkdir()
shutil.copy2(ROOT / "docs/THIRD-PARTY.md", licenses)
# The launcher's fallback font (src/m3_launcher_font.h) is drawn from Pixelify Sans.
shutil.copy2(ROOT / "android/app/src/main/res/raw/pixelify_sans_ofl.txt", licenses / "PixelifySans-OFL.txt")
cache = a.binary.parent / "CMakeCache.txt"
paths = {}
if cache.is_file():
    paths = dict(re.findall(r"^(GBARECOMP_ROOT|RECOMP_UI_ROOT):PATH=(.+)$", cache.read_text(), re.M))
engine = Path(paths.get("GBARECOMP_ROOT", ROOT / "gbarecomp"))
ui = Path(paths.get("RECOMP_UI_ROOT", ROOT / "recomp-ui"))
for name, source in [("gbarecomp", engine / "LICENSE"), ("recomp-ui", ui / "LICENSE"), ("SDL2", engine / "platform/android/third_party/SDL/LICENSE.txt")]:
    shutil.copy2(source, licenses / (name + "-LICENSE.txt"))
if macos:
    # The runtime resolves its data next to the executable; a bundle keeps data
    # in Resources, so expose it where the loader looks.
    for entry in ("assets", "game.toml"):
        (binary_dir / entry).symlink_to(f"../Resources/{entry}")
    make_icns(ROOT / "assets/img/Mother 3.ico", data_dir / "Mother3.icns")
    bundle_dylibs(binary_dir / "Mother3Recomp", app / "Contents/Frameworks")
    verify_links(binary_dir / "Mother3Recomp", app / "Contents/Frameworks")
    for library in (app / "Contents/Frameworks").iterdir():
        subprocess.run(["codesign", "--force", "--sign", "-", str(library)], check=True, capture_output=True)
    subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True, capture_output=True)
    subprocess.run(["codesign", "--verify", "--strict", str(app)], check=True, capture_output=True)
for file in stage.rglob("*"):
    if file.suffix.lower() in (".gba", ".bin", ".sav", ".p12", ".keystore", ".cpp", ".c", ".h") or file.name in ("rom.cfg", "bios.cfg"):
        raise SystemExit(f"Private or development file in package: {file}")
archive = stage.parent / (stage.name + (".tar.gz" if a.platform == "linux-x86_64" else ".zip"))
if a.platform == "linux-x86_64":
    with tarfile.open(archive, "w:gz") as output:
        output.add(stage, arcname=stage.name)
elif macos:
    # zipfile follows symlinks, which drops the app's data links; ditto keeps
    # the bundle layout that codesign verified.
    subprocess.run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent",
                    str(stage), str(archive)], check=True)
else:
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
        for file in stage.rglob("*"):
            if file.is_file():
                output.write(file, file.relative_to(stage.parent))
print(archive)
