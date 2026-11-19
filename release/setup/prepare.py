"""Deckrunner's one-time setup: builds everything Deckrunner reads from the player's own Skate 3 disc and Cyberpunk 2077
install, and installs it into the game folder. Nothing game-derived ships with Deckrunner, and no programs either (Nexus
Mods does not allow them in mod files): the programs setup runs come from Deckrunner's own GitHub release and from their
makers' official releases, each pinned and checked against its published hash.

    python setup/prepare.py --skate <Skate 3 ISO or extracted folder> --cyberpunk <Cyberpunk 2077 folder>
    python setup/prepare.py --cyberpunk <folder> --uninstall

Each stage leaves a receipt, so a rerun resumes after the last stage that finished (--redo STAGE reruns one and every
stage after it). Intermediates live in %LOCALAPPDATA%\\Deckrunner\\setup (--work for another place); --clean deletes
them after a successful run.

Stages:
  check       the game folder (Cyberpunk2077.exe and RED4ext)
  tools       skate-xma and xex_image (SK8V's, built unchanged), from Deckrunner's GitHub release
  disc      the 13 files Skate needs, from the disc (SK8V's xiso.py)
  convert     Skate's data (SK8V's convert-skate-data.py, running the donor engine's asset pipeline)
  audio       Skate's audio (SK8V's prepare-skate-audio.py)
  tuning      Skate's audio tuning tables (SK8V's prepare-audio-tuning.py)
  wolvenkit   WolvenKit's libraries: its official command-line release
  dotnet      a .NET 10 runtime for wkprobe: the installed one, else Microsoft's official runtime zip
  rigs        V's two player rigs, from the game's archives (wkprobe json)
  graph       V's animation graph with Deckrunner's pose input (wkprobe patch-skate, pack)
  board       Skate's skateboard as a game mesh (board_glb.py, wkprobe board-build, pack)
  install     the two archives into the game folder (the data is built in place; the plugin is Deckrunner's main file)
  verify      every file the plugin checks for, and a note if the plugin itself isn't there yet
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent            # <release>/setup
RELEASE = HERE.parent                             # <release>
WKPROBE = HERE / 'wkprobe' / 'wkprobe.dll'        # framework-dependent: run by a .NET 10 runtime
BOARD_GLB = HERE / 'board_glb.py'
PLUGIN_REL = Path('red4ext/plugins/deckrunner')
PLUGIN_MISSING = '''
   Note: the Deckrunner plugin is not in this game folder yet. Install Deckrunner's main file too (with Vortex or MO2,
   or extract it into the game folder). With MO2 it only appears while the game runs through MO2, so this is expected
   there.'''
ARCHIVE_REL = Path('archive/pc/mod')
ARCHIVES = ['deckrunner_c3.archive', 'deckrunner_board.archive']
SK8V_DIR = HERE / 'sk8v'                          # SK8V 1.1.10's setup scripts and donor snapshot, laid out as its repo
# The two programs SK8V's scripts run, built unchanged from SK8V 1.1.10, in Deckrunner's own GitHub release (Nexus Mods
# does not allow programs in mod files); GitHub's published SHA-256 for the file.
TOOLS = {
    'version': '1',
    'url': 'https://github.com/londonbrown/Deckrunner/releases/download/setup-tools-1/deckrunner-setup-tools-1.zip',
    'size': 321124,
    'sha256': 'c71d9cc17298f6c702d923ff9ce56caa4077e0c71ad215a8c22fa7f1bcf0789c',
}
# WolvenKit's command-line release; wkprobe links its libraries (both GPL-3.0). Pinned to the version wkprobe is built
# against; GitHub's published SHA-256 for the file.
WOLVENKIT = {
    'version': '10.0.0-beta.1',
    'url': 'https://github.com/WolvenKit/WolvenKit/releases/download/10.0.0-beta.1/WolvenKit.Console-10.0.0-beta.1.zip',
    'size': 44957672,
    'sha256': '292185c2f51d1ca85b95e1a7ca63a2d1bc8a3fc5ee387275e9a42ec78103155e',
}
# Microsoft's .NET 10 runtime (MIT), used only when no .NET 10 runtime is installed; Microsoft's published SHA-512.
DOTNET = {
    'version': '10.0.15',
    'url': 'https://builds.dotnet.microsoft.com/dotnet/Runtime/10.0.15/dotnet-runtime-10.0.15-win-x64.zip',
    'size': 36980907,
    'sha512': '06f0bedd59ae4ac722ada805a6d6bbf605bcdc790c71f59b7003878072902b5cde5dec8c40c0baa81192660d4748e9d79afdbdcb0c0ea9d6b821d0a044c4c6dd',
}
RIGS = {'player_woman_skeleton.rig.json': r'base\characters\entities\player\player_woman_skeleton.rig',
        'player_man_skeleton.rig.json': r'base\characters\entities\player\player_man_skeleton.rig'}
GRAPH = r'base\gameplay\anim_graphs\player_base.animgraph'
STEPS = {'check': 'Checking the game folder', 'tools': "Getting Deckrunner's setup tools", 'disc': 'Reading the Skate 3 disc',
         'convert': 'Converting Skate 3 data', 'audio': 'Converting Skate 3 audio', 'tuning': 'Reading audio tuning',
         'wolvenkit': 'Getting WolvenKit', 'dotnet': 'Finding .NET 10', 'rigs': "Exporting V's rigs",
         'graph': "Building V's animation graph", 'board': 'Building the skateboard',
         'install': 'Installing into the game folder', 'verify': 'Checking the installation'}


def file_hash(path, algo='sha256'):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, algo).hexdigest()


def download(pin, dest, algo='sha256', local=None):
    """The pinned file at dest, downloaded once and checked against its published hash. `local` (testing only) takes a
    local copy instead, without the hash check."""
    if local:
        shutil.copyfile(local, dest)
        return dest
    if dest.is_file() and dest.stat().st_size == pin['size'] and file_hash(dest, algo) == pin[algo]:
        return dest
    print(f'   downloading {pin["url"].rsplit("/", 1)[1]} ({pin["size"] / 1e6:.0f} MB)', flush=True)
    part = dest.with_suffix(dest.suffix + '.part')
    req = urllib.request.Request(pin['url'], headers={'User-Agent': 'Deckrunner-setup'})
    with urllib.request.urlopen(req) as r, open(part, 'wb') as f:
        shutil.copyfileobj(r, f, 1 << 20)
    got = file_hash(part, algo)
    if got != pin[algo]:
        part.unlink()
        raise SystemExit(f'{dest.name} does not match its published {algo.upper()} (got {got}); nothing was used')
    part.replace(dest)
    return dest


def run(*args, env=None):
    print('   $', ' '.join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], check=True, env=env)


class Prepare:
    def __init__(self, a):
        self.a = a
        self.cp = a.cyberpunk.resolve()
        self.plugin = self.cp / PLUGIN_REL
        self.data = self.plugin / 'data'
        self.work = (a.work or Path(os.environ.get('LOCALAPPDATA', Path.home())) / 'Deckrunner' / 'setup').resolve()
        self.disc = a.skate.resolve() if a.skate and a.skate.is_dir() else self.work / 'disc'
        self.skate = self.data / 'skate'
        self.assets = self.skate / 'assets'
        self.audio = self.skate / 'audio'
        self.rigs = self.data / 'rigs'
        self.sk8v_tools = SK8V_DIR / 'tools'
        self.tools = self.work / f'tools-{TOOLS["version"]}'  # skate-xma.exe, xex_image.exe
        self.wk = a.wolvenkit.resolve() if a.wolvenkit else self.work / f'wolvenkit-{WOLVENKIT["version"]}'
        self.dotnet_file = self.work / '.dotnet'  # which dotnet runs wkprobe (the dotnet stage writes it)
        self.receipts = self.work / '.stages'

    def py(self, script, *args):
        run(sys.executable, '-B', script, *args)

    def wkprobe(self, *args):
        env = os.environ.copy()
        env.update(DKR_WOLVENKIT=str(self.wk), DKR_GAME=str(self.cp))
        run(self.dotnet_file.read_text(encoding='utf-8').strip(), WKPROBE, *args, env=env)

    # name, files whose change invalidates it, what it reads, function
    def stages(self):
        disc_id = self.input_id(self.a.skate) if self.a.skate else ''
        game_id = self.input_id(self.cp / 'bin/x64/Cyberpunk2077.exe')
        t = self.sk8v_tools
        tools = TOOLS['sha256']
        return [
            ('check', [], [game_id], self.stage_check),
            ('tools', [], [tools], self.stage_tools),
            ('disc', [t / 'xiso.py'], [disc_id], self.stage_disc),
            ('convert', [t / 'convert-skate-data.py'], [disc_id], self.stage_convert),
            ('audio', [t / 'prepare-skate-audio.py'], [tools, disc_id], lambda: self.py(
                t / 'prepare-skate-audio.py', '--skate-data', self.disc, '--out', self.audio, '--decoder', self.tools / 'skate-xma.exe')),
            ('tuning', [t / 'prepare-audio-tuning.py'], [tools, disc_id], lambda: self.py(
                t / 'prepare-audio-tuning.py', '--collections', self.assets / 'private/stock/skater-collections.json',
                '--out', self.audio / 'grain-tuning.bin', '--component-out', self.audio / 'component-tuning.bin',
                '--xex', self.disc / 'default.xex', '--xex-tool', self.tools / 'xex_image.exe')),
            ('wolvenkit', [], [str(self.wk), WOLVENKIT['sha256']], self.stage_wolvenkit),
            ('dotnet', [], [str(self.a.dotnet), self.a.fetch_dotnet], self.stage_dotnet),
            ('rigs', [WKPROBE], [game_id], self.stage_rigs),
            ('graph', [WKPROBE], [game_id], self.stage_graph),
            ('board', [WKPROBE, BOARD_GLB], [game_id, disc_id], self.stage_board),
            ('install', [], [game_id], self.stage_install),
            ('verify', [], [], self.stage_verify),
        ]

    @staticmethod
    def input_id(p):
        p = Path(p)
        if not p.exists():
            return str(p)
        st = p.stat()
        return f'{p.resolve()}|{st.st_size}|{int(st.st_mtime)}'

    def key(self, files, inputs):
        h = hashlib.sha256()
        for s in files:
            h.update(file_hash(s).encode() if Path(s).is_file() else str(s).encode())
        h.update(json.dumps(inputs).encode())
        h.update(str(self.cp).lower().encode())  # where its output went: another game folder redoes every stage
        return h.hexdigest()

    # ----- stages -----
    def stage_check(self):
        exe = self.cp / 'bin/x64/Cyberpunk2077.exe'
        if not exe.is_file():
            raise SystemExit(f'{self.cp} is not a Cyberpunk 2077 folder (no bin\\x64\\Cyberpunk2077.exe)')
        if not (self.cp / 'red4ext/RED4ext.dll').is_file():
            raise SystemExit('RED4ext is not installed in this game folder. Install it first (Nexus mod 2380), then run setup again.')
        if not (self.cp / 'bin/x64/oo2ext_7_win64.dll').is_file():
            raise SystemExit('the game folder has no bin\\x64\\oo2ext_7_win64.dll (needed to read its archives)')

    def stage_tools(self):
        if (self.tools / 'skate-xma.exe').is_file() and (self.tools / 'xex_image.exe').is_file():
            return
        z = download(TOOLS, self.work / TOOLS['url'].rsplit('/', 1)[1])
        self.unzip(z, self.tools)
        for exe in ('skate-xma.exe', 'xex_image.exe'):
            if not (self.tools / exe).is_file():
                raise SystemExit(f'{z.name} has no {exe}')

    def stage_disc(self):
        sys.path.insert(0, str(self.sk8v_tools))
        import xiso
        if self.a.skate.is_dir():
            missing = [p for p in xiso.NEEDED if not (self.disc / p).is_file()]
            if missing:
                raise SystemExit(f'{self.disc} is not an extracted Skate 3 disc: missing {", ".join(missing)}')
            return
        xiso.extract_needed(self.a.skate, self.disc)

    def stage_convert(self):
        # convert-skate-data.py replaces its whole --out folder, so it builds in the work folder and only its assets
        # are moved into the plugin's data (the audio beside them is another stage's).
        out = self.work / 'skate-data'
        self.py(self.sk8v_tools / 'convert-skate-data.py', '--xex', self.disc / 'default.xex', '--out', out)
        shutil.rmtree(self.assets, ignore_errors=True)
        self.assets.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(out / 'assets'), str(self.assets))
        shutil.rmtree(out, ignore_errors=True)

    def stage_wolvenkit(self):
        if self.a.wolvenkit:
            if not (self.wk / 'WolvenKit.RED4.dll').is_file():
                raise SystemExit(f'{self.wk} has no WolvenKit.RED4.dll')
            return
        if (self.wk / 'WolvenKit.RED4.dll').is_file():
            return
        z = download(WOLVENKIT, self.work / WOLVENKIT['url'].rsplit('/', 1)[1])
        self.unzip(z, self.wk)

    def stage_dotnet(self):
        """An installed .NET 10 runtime when there is one; otherwise Microsoft's runtime zip, unpacked privately."""
        exe = None
        if self.a.dotnet:
            exe = str(self.a.dotnet)
        elif not self.a.fetch_dotnet:
            found = shutil.which('dotnet')
            if found:
                out = subprocess.run([found, '--list-runtimes'], capture_output=True, text=True).stdout
                if re.search(r'^Microsoft\.NETCore\.App 10\.', out, re.M):
                    exe = found
        if not exe:
            d = self.work / f'dotnet-{DOTNET["version"]}'
            if not (d / 'dotnet.exe').is_file():
                z = download(DOTNET, self.work / DOTNET['url'].rsplit('/', 1)[1], algo='sha512')
                self.unzip(z, d)
            exe = str(d / 'dotnet.exe')
        print(f'   wkprobe runs on {exe}', flush=True)
        self.dotnet_file.write_text(exe, encoding='utf-8')

    def unzip(self, z, dest):
        tmp = dest.with_name(dest.name + '.tmp')
        shutil.rmtree(tmp, ignore_errors=True)
        with zipfile.ZipFile(z) as zf:
            zf.extractall(tmp)
        shutil.rmtree(dest, ignore_errors=True)
        tmp.replace(dest)
        z.unlink()

    def stage_rigs(self):
        self.rigs.mkdir(parents=True, exist_ok=True)
        for out, depot in RIGS.items():
            self.wkprobe('json', depot, self.rigs / out)

    def stage_graph(self):
        w = self.work / 'graph'
        shutil.rmtree(w, ignore_errors=True)
        orig, patched = w / 'player_base.orig.animgraph', w / 'archive' / GRAPH.replace('\\', '/')
        w.mkdir(parents=True)
        self.wkprobe('raw', GRAPH, orig)
        self.wkprobe('patch-skate', orig, patched)
        self.wkprobe('pack', w / 'archive', w / ARCHIVES[0])

    def stage_board(self):
        w = self.work / 'board'
        shutil.rmtree(w, ignore_errors=True)
        raw = w / 'raw'
        self.py(BOARD_GLB, self.assets / 'private/skater.glb', raw / 'deckrunner/board')
        self.wkprobe('board-build', raw, w / 'archive')
        self.wkprobe('pack', w / 'archive', w / ARCHIVES[1])

    def stage_install(self):
        """The two archives built here; the data is already in place. Every file written
        outside data\\ is listed in installed.json, for --uninstall."""
        # The plugin itself comes from Deckrunner's main file (Vortex, MO2 or extracted by hand), never from setup.
        installed = []
        mod = self.cp / ARCHIVE_REL
        mod.mkdir(parents=True, exist_ok=True)
        for name, w in zip(ARCHIVES, ('graph', 'board')):
            shutil.copy2(self.work / w / name, mod / name)
            installed.append(str(ARCHIVE_REL / name))
        (self.plugin / 'installed.json').write_text(json.dumps({'files': installed, 'data': str(PLUGIN_REL / 'data')}, indent=1),
                                                     encoding='utf-8')

    def stage_verify(self):
        need = [self.assets / 'private/game.json', self.assets / 'private/skater.glb', self.audio / 'manifest.json',
                *(self.rigs / r for r in RIGS), *(self.cp / ARCHIVE_REL / n for n in ARCHIVES)]
        missing = [str(p) for p in need if not p.is_file()]
        if missing:
            raise SystemExit('setup finished, but these are missing:\n  ' + '\n  '.join(missing))
        if not (self.plugin / 'deckrunner.dll').is_file():
            print(PLUGIN_MISSING, flush=True)

    # ----- the run -----
    def run(self):
        self.work.mkdir(parents=True, exist_ok=True)
        self.receipts.mkdir(parents=True, exist_ok=True)
        stages = self.stages()
        names = [s[0] for s in stages]
        if self.a.redo and self.a.redo not in names:
            raise SystemExit(f'--redo: no stage {self.a.redo} (stages: {", ".join(names)})')
        redo_from = names.index(self.a.redo) if self.a.redo else None
        t0 = time.time()
        for i, (name, files, inputs, fn) in enumerate(stages):
            receipt = self.receipts / f'{name}.json'
            key = self.key(files, inputs)
            done = receipt.is_file() and json.loads(receipt.read_text(encoding='utf-8')).get('key') == key
            if done and (redo_from is None or i < redo_from):
                print(f'== [{i + 1}/{len(stages)}] {STEPS[name]}: done before', flush=True)
                continue
            if name in ('disc', 'convert', 'audio', 'tuning', 'board') and not self.a.skate:
                raise SystemExit('--skate is needed (your Skate 3 ISO or extracted disc folder)')
            print(f'== [{i + 1}/{len(stages)}] {STEPS[name]}', flush=True)
            s0 = time.time()
            fn()
            receipt.write_text(json.dumps({'key': key, 'seconds': round(time.time() - s0, 1)}), encoding='utf-8')
        if self.a.clean:
            for d in ('disc', 'graph', 'board'):
                if d != 'disc' or not (self.a.skate and self.a.skate.is_dir()):
                    shutil.rmtree(self.work / d, ignore_errors=True)
        print(f'== Deckrunner is set up ({time.time() - t0:.0f} s). Start the game, load a save and press Ctrl+Shift+F10 to skate.', flush=True)

    def uninstall(self):
        manifest = self.plugin / 'installed.json'
        if not manifest.is_file():
            raise SystemExit(f'no Deckrunner installation in {self.cp} (no {PLUGIN_REL}\\installed.json)')
        m = json.loads(manifest.read_text(encoding='utf-8'))
        for rel in m['files']:
            if Path(rel).name.lower() == 'deckrunner.ini':
                continue  # the player's settings (0.1.x manifests list it)
            (self.cp / rel).unlink(missing_ok=True)
        shutil.rmtree(self.cp / m['data'], ignore_errors=True)
        manifest.unlink()
        shutil.rmtree(self.receipts, ignore_errors=True)
        print(f'Removed what setup built from {self.cp}: the data and the two archives. Remove the plugin itself (the '
              'Deckrunner main file) with your mod manager, or delete red4ext\\plugins\\deckrunner by hand.')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--skate', type=Path, help='Skate 3 (Xbox 360) disc image, or its extracted folder')
    ap.add_argument('--cyberpunk', type=Path, required=True, help='the Cyberpunk 2077 folder (holds bin\\x64\\Cyberpunk2077.exe)')
    ap.add_argument('--work', type=Path, help='where intermediates go (default %%LOCALAPPDATA%%\\Deckrunner\\setup)')
    ap.add_argument('--wolvenkit', type=Path, help='use this WolvenKit folder instead of downloading the pinned release')
    ap.add_argument('--dotnet', type=Path, help='run wkprobe with this dotnet.exe')
    ap.add_argument('--fetch-dotnet', action='store_true', help="use Microsoft's runtime zip even when .NET 10 is installed")
    ap.add_argument('--redo', help='rerun this stage and every stage after it')
    ap.add_argument('--clean', action='store_true', help='afterwards delete the intermediates')
    ap.add_argument('--uninstall', action='store_true', help='remove Deckrunner from the game folder')
    a = ap.parse_args()
    if sys.version_info < (3, 11):
        raise SystemExit('Python 3.11 or newer is needed')
    p = Prepare(a)
    p.uninstall() if a.uninstall else p.run()


if __name__ == '__main__':
    main()
