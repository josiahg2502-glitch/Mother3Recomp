"""Stage a player package without ROMs, BIOS dumps, sources or personal settings."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("version")
p.add_argument("platform", choices=["windows-x64", "linux-x86_64"])
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
shutil.copy2(a.binary, stage / ("Mother3Recomp.exe" if windows else "Mother3Recomp"))
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
    (stage / "Mother3Recomp").chmod(0o755)
    launcher = stage / "run_mother3.sh"
    launcher.write_text('#!/bin/sh\ncd -- "$(dirname -- "$0")" || exit 1\nexec ./Mother3Recomp "$@"\n')
    launcher.chmod(0o755)
shutil.copytree(a.binary.parent / "assets/fonts", stage / "assets/fonts")
images = stage / "assets/img"
images.mkdir(parents=True)
for image in (ROOT / "assets/img").iterdir():
    if image.suffix == ".tga" or image.name in ("flags.png", "icon.png"):
        shutil.copy2(image, images / image.name)
config = (ROOT / "game.toml").read_text(encoding="utf-8")
for section in ("bios", "rom"):
    config = re.sub(r'(\[' + section + r'\]\s*\n)path = "[^"\n]*"\n', r'\1', config)
(stage / "game.toml").write_text(config, encoding="utf-8")
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
for file in stage.rglob("*"):
    if file.suffix.lower() in (".gba", ".bin", ".sav", ".p12", ".keystore", ".cpp", ".c", ".h") or file.name in ("rom.cfg", "bios.cfg"):
        raise SystemExit(f"Private or development file in package: {file}")
archive = stage.parent / (stage.name + (".zip" if windows else ".tar.gz"))
if windows:
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
        for file in stage.rglob("*"):
            if file.is_file():
                output.write(file, file.relative_to(stage.parent))
else:
    with tarfile.open(archive, "w:gz") as output:
        output.add(stage, arcname=stage.name)
print(archive)
