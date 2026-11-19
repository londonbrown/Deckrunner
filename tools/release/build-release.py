"""Builds the Deckrunner release from an allowlist, as two zips (issue #1): Deckrunner-<version>.zip, the plugin and the
runtime with the game folder as its root, for mod managers (no scripts, so Nexus Mods passes it on its own); and
Deckrunner-Setup-<version>.zip, the setup and wkprobe (GPL-3.0, with its source). No programs: Nexus Mods quarantines uploads that contain executables (0.1.0 was), so the tools setup
runs (Deckrunner's setup tools, WolvenKit, .NET, Python) are downloaded by the setup from their official releases,
pinned and hash-checked, and wkprobe ships as a .NET assembly. No PowerShell either: VirusTotal's sandbox flagged 0.1.1's
launcher (powershell -ExecutionPolicy Bypass -File from the Temp folder it unpacks into) under four Sigma rules, so the
launcher is a .cmd that runs setup.py. Nothing game-derived either: every file is listed here, and a final scan refuses
game data formats, programs, script-host files and this machine's paths.

    python tools/release/build-release.py --version 0.1.2 [--engine <deps/deckrunner-engine>] [--skip-wkprobe]

Build first: the plugin (cmake --build <probe-build> --config Release) and the runtime with paths remapped (BACKLOG.md,
"Release 0.1.0"). wkprobe is published here (dotnet publish, framework-dependent, no apphost, against the WolvenKit
folder in WOLVENKIT_DIR). Output: build/release/Deckrunner-<version>/, its .zip and SHA256SUMS.txt.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

import scan

REPO = Path(__file__).resolve().parents[2]
LOCAL = Path(os.environ.get('LOCALAPPDATA', '')) / 'Deckrunner'
PLUGIN_DLL = LOCAL / 'probe-build/Release/deckrunner_probe.dll'
RUNTIME_DLL = LOCAL / 'cargo-release/release/deckrunner_runtime.dll'
WKPROBE = REPO / 'tools/wkprobe'
GPL3 = Path(__file__).with_name('licenses') / 'GPL-3.0.txt'  # the canonical GNU GPL v3 text (gnu.org gpl-3.0.txt)
GPL3_SHA = '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986'
WKPROBE_FILES = ['wkprobe.dll', 'wkprobe.deps.json', 'wkprobe.runtimeconfig.json']
# What the plugin file (Deckrunner's main file on Nexus) may hold: the two DLLs, the ini and text.
PLUGIN_SUFFIXES = {'.dll', '.ini', '.md', '.txt', '', '.yaml', '.xl', '.archive', '.reds'}
# #3: the skateboard in the weapon wheel, for players with TweakXL and ArchiveXL: its record (r6\tweaks) and its name
# and description, an archive built here by wkprobe from Deckrunner's own JSON (the one archive a release may hold).
ITEMS_ARCHIVE = 'archive/pc/mod/deckrunner_items.archive'
ITEMS_SOURCES = REPO / 'release/plugin/archive/deckrunner_items'
# SK8V 1.1.10's setup scripts (Apache-2.0), shipped as Deckrunner's own copies; their two programs come from Deckrunner's
# GitHub release (build-tools.py). The donor tools they run, as SK8V's own release lists them (traced there with an
# open() audit hook), at the pinned donor commit.
SK8V_TOOLS = ['xiso.py', '_common.py', 'convert-skate-data.py', 'prepare-skate-audio.py', 'prepare-audio-tuning.py']
SK8V_FILES = ['LICENSE', 'NOTICE', 'upstreams.lock.json']
DONOR = 'upstream/skate-3-rust-engine-donor'
DONOR_PATHS = ['tools/asset_pipeline', 'tools/owned_game', 'tools/vendor/skate3_anim', 'tools/vendor/skate3_ui',
               'tools/vendor/utt', 'tools/default_skater_retail_manifest.json', 'tools/extract_default_skater.py',
               'tools/extract_session_marker.py', 'tools/install_prepared_hud.py', 'tools/prepare_hud.py',
               'tools/prepare_runtime_huds.py']
DONOR_COMMIT = 'cb7968930f14dad38457e98720d1a274e469eec2'
# deps/deckrunner-engine: Deckrunner's fork of SK8V 1.1.10, its pins pointed at Deckrunner's forks, with overlay patch
# 0050 (the session marker for the HUD; no setup file changes).
ENGINE_COMMIT = '373fd2d9c9a56148d0aa0d2fa13a22a72b8ebc43'


def copy(src, dst):
    src = Path(src)
    if not src.is_file():
        raise SystemExit(f'missing {src}')
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def sha256(p):
    with open(p, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def publish_wkprobe(out):
    if not os.environ.get('WOLVENKIT_DIR'):
        raise SystemExit('set WOLVENKIT_DIR to a WolvenKit 10.0.0-beta.1 folder (wkprobe builds against its libraries)')
    pub = LOCAL / 'wkprobe-publish'
    shutil.rmtree(pub, ignore_errors=True)
    subprocess.run(['dotnet', 'publish', str(WKPROBE / 'wkprobe.csproj'), '-c', 'Release', '-p:UseAppHost=false',
                    '-p:DebugType=none', '-p:PathMap=' + f'{REPO}=deckrunner', '-o', str(pub), '-v', 'q', '-nologo'], check=True)
    for f in WKPROBE_FILES:
        copy(pub / f, out / f)
    copy(GPL3, out / 'LICENSE')
    for f in sorted(WKPROBE.glob('*.cs')) + [WKPROBE / 'wkprobe.csproj']:
        copy(f, out / 'source' / f.name)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version', required=True)
    ap.add_argument('--engine', type=Path, default=REPO / 'deps' / 'deckrunner-engine',
                    help="Deckrunner's fork of SK8V (the deps/deckrunner-engine submodule), with its upstreams fetched")
    ap.add_argument('--skip-wkprobe', action='store_true', help='reuse the last publish (for repackaging)')
    ap.add_argument('--plugin-dll', type=Path, default=PLUGIN_DLL, help='the built deckrunner_probe.dll')
    ap.add_argument('--runtime-dll', type=Path, default=RUNTIME_DLL, help='the built deckrunner_runtime.dll')
    ap.add_argument('--update-setup-version', action='store_true',
                    help='the setup changed: bump release/setup/VERSION to the next number and record its hash')
    a = ap.parse_args()
    if sha256(GPL3) != GPL3_SHA:
        raise SystemExit(f'{GPL3} is not the canonical GPL-3.0 text')

    # Two files (issue #1): the plugin, which mod managers install and Nexus Mods passes on its own, and the setup,
    # whose scripts Nexus holds for a moderator but which rarely changes.
    rel = REPO / 'release'
    setup_number, setup_hash = read_setup_version()
    plugin_out = REPO / 'build/release' / f'Deckrunner-{a.version}'
    out = REPO / 'build/release' / f'Deckrunner-Setup-{setup_number}'
    for d in (plugin_out, out):
        shutil.rmtree(d, ignore_errors=True)
    plugin = plugin_out / 'red4ext/plugins/deckrunner'
    copy(a.plugin_dll, plugin / 'deckrunner.dll')
    copy(a.runtime_dll, plugin / 'runtime/deckrunner_runtime.dll')
    # Never deckrunner.ini itself: an update would overwrite the player's settings. The plugin makes it from this.
    copy(rel / 'deckrunner.ini', plugin / 'deckrunner.default.ini')
    copy(rel / 'plugin/README.md', plugin / 'README.md')
    copy(rel / 'plugin/r6/tweaks/deckrunner/skateboard.yaml', plugin_out / 'r6/tweaks/deckrunner/skateboard.yaml')
    # The scripts: DeckrunnerHud's natives, the weapon wheel's pick while skating (redscript) and the HUD (Codeware).
    for f in sorted((rel / 'plugin/r6/scripts/deckrunner').glob('*.reds')):
        copy(f, plugin_out / 'r6/scripts/deckrunner' / f.name)
    copy(rel / 'plugin/archive/deckrunner_items.archive.xl', plugin_out / (ITEMS_ARCHIVE + '.xl'))
    for f in ('LICENSE', 'NOTICE'):
        copy(rel / f, plugin / f)
    for f in ('Deckrunner Setup.cmd', 'Deckrunner Uninstall.cmd', 'README.md', 'LICENSE', 'NOTICE'):
        copy(rel / f, out / f)
    copy(REPO / 'media/deckrunner-logo.png', out / 'deckrunner-logo.png')
    setup = out / 'setup'
    for f in ('prepare.py', 'setup.py'):
        copy(rel / 'setup' / f, setup / f)
    copy(REPO / 'tools/board/board_glb.py', setup / 'board_glb.py')
    for f in ('Deckrunner Setup.cmd', 'Deckrunner Uninstall.cmd'):
        text = (out / f).read_bytes()
        code = [ln for ln in text.lower().split(b'\r\n') if not ln.strip().startswith(b'rem ')]
        if any(b'powershell' in ln for ln in code) or b'\r\n' not in text or text.replace(b'\r\n', b'').count(b'\n'):
            raise SystemExit(f'{f}: it must not run PowerShell, and it needs CRLF line endings (cmd mis-reads LF labels)')
    engine = subprocess.run(['git', '-C', str(a.engine), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    donor = a.engine / DONOR
    head = subprocess.run(['git', '-C', str(donor), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    if engine != ENGINE_COMMIT or head != DONOR_COMMIT:
        raise SystemExit(f'{a.engine} is at {engine[:8]} with donor {head[:8]}; need {ENGINE_COMMIT[:8]} with donor {DONOR_COMMIT[:8]}')
    sk = setup / 'sk8v'
    for f in SK8V_TOOLS:
        copy(a.engine / 'tools' / f, sk / 'tools' / f)
    for f in SK8V_FILES:
        copy(a.engine / f, sk / f)
    for p in DONOR_PATHS:
        src = donor / p
        for f in ([x for x in src.rglob('*') if x.is_file()] if src.is_dir() else [src]):
            if '__pycache__' not in f.parts and f.suffix != '.pyc':
                copy(f, sk / DONOR / f.relative_to(donor))
    (sk / DONOR / '.sk8v-pin').write_text(DONOR_COMMIT + '\n', encoding='ascii')
    if a.skip_wkprobe:
        for f in (LOCAL / 'wkprobe-release').rglob('*'):
            if f.is_file():
                copy(f, setup / 'wkprobe' / f.relative_to(LOCAL / 'wkprobe-release'))
    else:
        publish_wkprobe(setup / 'wkprobe')
        shutil.rmtree(LOCAL / 'wkprobe-release', ignore_errors=True)
        shutil.copytree(setup / 'wkprobe', LOCAL / 'wkprobe-release')

    build_items_archive(setup / 'wkprobe' / 'wkprobe.dll', plugin_out / ITEMS_ARCHIVE)
    subprocess.run([sys.executable, '-I', str(Path(__file__).with_name('notices.py')), '--out', str(plugin / 'THIRD-PARTY-NOTICES.txt')],
                   check=True)
    if any(f.suffix.lower() not in PLUGIN_SUFFIXES for f in plugin_out.rglob('*') if f.is_file()):
        raise SystemExit(f'the plugin file may only hold {sorted(PLUGIN_SUFFIXES)}: no scripts, so Nexus Mods passes it')
    # The setup has its own version (release/setup/VERSION), so a plugin release doesn't republish an unchanged setup:
    # Nexus Mods holds every setup upload for a moderator. Its hash covers everything in the setup zip but wkprobe's
    # build output, which differs from build to build; a changed setup must get a new number.
    now = setup_content_hash(out)
    if now != setup_hash:
        if not a.update_setup_version:
            raise SystemExit(f'the setup changed since Deckrunner Setup {setup_number} (release/setup/VERSION); rerun with '
                             '--update-setup-version to make it the next number, and commit release/setup/VERSION')
        if setup_hash:  # a recorded setup changed: the next number; the first record keeps its number
            setup_number += 1
            moved = out.with_name(f'Deckrunner-Setup-{setup_number}')
            shutil.rmtree(moved, ignore_errors=True)
            out = out.rename(moved)
        SETUP_VERSION.write_text(f'{setup_number}\n{now}\n', encoding='ascii', newline='\n')
        print(f'release/setup/VERSION is now Deckrunner Setup {setup_number}')
    finish(plugin_out, top=None, allow=[plugin_out / ITEMS_ARCHIVE])  # its root is the game folder, for Vortex and MO2
    finish(out, top=out.name)


SETUP_VERSION = REPO / 'release/setup/VERSION'


def read_setup_version():
    """Deckrunner Setup's own number and the hash of its contents, from release/setup/VERSION."""
    lines = SETUP_VERSION.read_text(encoding='ascii').split()
    return int(lines[0]), (lines[1] if len(lines) > 1 else '')


def setup_content_hash(out):
    """SHA-256 over the setup folder's files (path and text with LF line endings), leaving out wkprobe's build
    output and the checksum list."""
    h = hashlib.sha256()
    for f in sorted(out.rglob('*')):
        rel_path = f.relative_to(out).as_posix()
        if not f.is_file() or rel_path == 'SHA256SUMS.txt' or rel_path in {f'setup/wkprobe/{n}' for n in WKPROBE_FILES}:
            continue
        h.update(rel_path.encode() + b'\0' + f.read_bytes().replace(b'\r\n', b'\n') + b'\0')
    return h.hexdigest()


def build_items_archive(wkprobe, archive):
    """The weapon wheel's name archive: each <name>.json.json under ITEMS_SOURCES (WolvenKit JSON) becomes the resource
    <name>.json at the same depot path, and wkprobe packs them (and reads the archive back: every file there, decoding to
    what went in). No game folder is needed: without the game's Oodle the archive is written uncompressed."""
    if not os.environ.get('WOLVENKIT_DIR'):
        raise SystemExit('set WOLVENKIT_DIR to a WolvenKit 10.0.0-beta.1 folder (wkprobe builds the weapon wheel archive)')
    work = LOCAL / 'items-archive'
    shutil.rmtree(work, ignore_errors=True)
    (work / 'nogame').mkdir(parents=True)
    env = {**os.environ, 'DKR_WOLVENKIT': os.environ['WOLVENKIT_DIR'], 'DKR_GAME': str(work / 'nogame')}
    sources = sorted(ITEMS_SOURCES.rglob('*.json.json'))
    if not sources:
        raise SystemExit(f'no sources in {ITEMS_SOURCES}')
    for src in sources:
        out = work / 'depot' / src.relative_to(ITEMS_SOURCES).with_suffix('')
        subprocess.run(['dotnet', str(wkprobe), 'cr2w', str(src), str(out)], env=env, check=True)
    archive.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['dotnet', str(wkprobe), 'pack', str(work / 'depot'), str(archive)], env=env, check=True)


def finish(out, top, allow=()):
    """Scans a release folder, writes its SHA256SUMS.txt and zips it, under `top` or at the zip's root."""
    problems = scan.scan([out], allow)
    if problems:
        raise SystemExit('the release holds what it must not:\n  ' + '\n  '.join(problems))
    sums = [f'{sha256(f)}  {f.relative_to(out).as_posix()}' for f in sorted(out.rglob('*')) if f.is_file()]
    if top:
        (out / 'SHA256SUMS.txt').write_text('\n'.join(sums) + '\n', encoding='utf-8')
    z = out.parent / f'{out.name}.zip'
    z.unlink(missing_ok=True)
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for f in sorted(out.rglob('*')):
            if f.is_file():
                zf.write(f, f'{top}/{f.relative_to(out).as_posix()}' if top else f.relative_to(out).as_posix())
    size = sum(f.stat().st_size for f in out.rglob('*') if f.is_file())
    print(f'{z} ({z.stat().st_size / 1e6:.1f} MB zipped, {size / 1e6:.1f} MB unpacked, {len(sums)} files; scan clean)')

if __name__ == '__main__':
    main()
