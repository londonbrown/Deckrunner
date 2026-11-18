"""Deckrunner setup: finds Python 3.11+ with numpy and Pillow (or offers a private copy), finds Cyberpunk 2077 (Steam or
GOG, else asks), asks for the player's own Skate 3 disc image or extracted folder, and runs prepare.py.

"Deckrunner Setup.cmd" starts it on any Python 3 it finds; with none at all, the .cmd downloads the embeddable Python
first and this script adds numpy and Pillow. Plain Python, Windows' own dialogs and console prompts: no PowerShell, whose
script runs from a download folder antivirus heuristics flag. Every download asks first, on the console.

    python -B -s setup\\setup.py [--uninstall] [--check-python]

--check-python stops once Python is ready (for testing the download); --probe and --check are the .cmd's and this
script's own interpreter tests. Runs on Python 3.6+, so it can replace an older Python with the private copy.
"""
import ctypes
import hashlib
import os
import re
import shutil
import subprocess
import sys
import traceback
import urllib.request
import zipfile
from ctypes import wintypes
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PRIVATE = ROOT / 'python'
PRIVATE_EXE = PRIVATE / 'python.exe'
STAGE = ROOT / 'python.download'
# SK8V 1.1.10's pins (its tools\get-python.ps1): python.org's embeddable Python and the numpy and Pillow wheels the
# donor tools use. "Deckrunner Setup.cmd" pins the first one too, for when there is no Python to run this script.
PINS = [
    ('https://www.python.org/ftp/python/3.13.15/python-3.13.15-embed-amd64.zip',
     'd1f04d990aee1253d8569e8e5104e30fa9f5fa830899f14843448872d936a2cf'),
    ('https://files.pythonhosted.org/packages/cb/3b/d58c12eafcb298d4e6d0d40216866ab15f59e55d148a5658bb3132311fcf/'
     'numpy-2.2.6-cp313-cp313-win_amd64.whl',
     'b0544343a702fa80c95ad5d3d608ea3599dd54d4632df855e4c8d24eb6ecfa1c'),
    ('https://files.pythonhosted.org/packages/23/85/397c73524e0cd212067e0c969aa245b01d50183439550d24d9f55781b776/'
     'pillow-11.3.0-cp313-cp313-win_amd64.whl',
     '0bce5c4fd0921f99d2e858dc4d4d64193407e1b99478bc5cacecba2311abde51'),
]
TITLE = 'Deckrunner setup'

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
user32 = ctypes.WinDLL('user32', use_last_error=True)
shell32 = ctypes.WinDLL('shell32', use_last_error=True)
ole32 = ctypes.WinDLL('ole32', use_last_error=True)
comdlg32 = ctypes.WinDLL('comdlg32', use_last_error=True)
kernel32.GetConsoleWindow.restype = wintypes.HWND
kernel32.GetStdHandle.restype = wintypes.HANDLE
kernel32.GetStdHandle.argtypes = [wintypes.DWORD]
kernel32.GetConsoleMode.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
kernel32.SetConsoleMode.argtypes = [wintypes.HANDLE, wintypes.DWORD]
user32.MessageBoxW.argtypes = [wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.UINT]
shell32.SHBrowseForFolderW.restype = ctypes.c_void_p
shell32.SHGetPathFromIDListW.argtypes = [ctypes.c_void_p, wintypes.LPWSTR]
ole32.CoTaskMemFree.argtypes = [ctypes.c_void_p]
ole32.CoInitialize.argtypes = [ctypes.c_void_p]
comdlg32.CommDlgExtendedError.restype = wintypes.DWORD


class BROWSEINFOW(ctypes.Structure):
    _fields_ = [('hwndOwner', wintypes.HWND), ('pidlRoot', ctypes.c_void_p), ('pszDisplayName', wintypes.LPWSTR),
                ('lpszTitle', wintypes.LPCWSTR), ('ulFlags', wintypes.UINT), ('lpfn', ctypes.c_void_p),
                ('lParam', wintypes.LPARAM), ('iImage', ctypes.c_int)]


class OPENFILENAMEW(ctypes.Structure):
    _fields_ = [('lStructSize', wintypes.DWORD), ('hwndOwner', wintypes.HWND), ('hInstance', wintypes.HINSTANCE),
                ('lpstrFilter', ctypes.c_void_p), ('lpstrCustomFilter', wintypes.LPWSTR),
                ('nMaxCustFilter', wintypes.DWORD), ('nFilterIndex', wintypes.DWORD), ('lpstrFile', wintypes.LPWSTR),
                ('nMaxFile', wintypes.DWORD), ('lpstrFileTitle', wintypes.LPWSTR), ('nMaxFileTitle', wintypes.DWORD),
                ('lpstrInitialDir', wintypes.LPCWSTR), ('lpstrTitle', wintypes.LPCWSTR), ('Flags', wintypes.DWORD),
                ('nFileOffset', wintypes.WORD), ('nFileExtension', wintypes.WORD), ('lpstrDefExt', wintypes.LPCWSTR),
                ('lCustData', wintypes.LPARAM), ('lpfnHook', ctypes.c_void_p), ('lpTemplateName', wintypes.LPCWSTR),
                ('pvReserved', ctypes.c_void_p), ('dwReserved', wintypes.DWORD), ('FlagsEx', wintypes.DWORD)]


comdlg32.GetOpenFileNameW.argtypes = [ctypes.POINTER(OPENFILENAMEW)]
shell32.SHBrowseForFolderW.argtypes = [ctypes.POINTER(BROWSEINFOW)]

IDYES, IDNO, IDCANCEL = 6, 7, 2
MB_YESNOCANCEL, MB_ICONQUESTION, MB_SETFOREGROUND = 0x3, 0x20, 0x10000


class Stop(Exception):
    pass


def color(code, text):
    return f'\x1b[{code}m{text}\x1b[0m' if VT else text


def say(text):
    print(color('96', text), flush=True)


def fail(text):
    raise Stop(text)


def enable_vt():
    h = kernel32.GetStdHandle(wintypes.DWORD(-11 & 0xFFFFFFFF))
    mode = wintypes.DWORD()
    return bool(kernel32.GetConsoleMode(h, ctypes.byref(mode)) and kernel32.SetConsoleMode(h, mode.value | 0x4))


def ask(question):
    """A console yes/no: downloads are agreed to by typing, never by a click."""
    try:
        return input(color('96', f'{question} [y/N] ')).strip().lower() in ('y', 'yes')
    except EOFError:
        return False


def owner():
    return kernel32.GetConsoleWindow()


def message(text, buttons):
    return user32.MessageBoxW(owner(), text, TITLE, buttons | MB_ICONQUESTION | MB_SETFOREGROUND)


def pick_folder(title):
    ole32.CoInitialize(None)
    name = ctypes.create_unicode_buffer(260)
    info = BROWSEINFOW(owner(), None, ctypes.cast(name, wintypes.LPWSTR), title,
                       0x1 | 0x40, None, 0, 0)  # BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE
    pidl = shell32.SHBrowseForFolderW(ctypes.byref(info))
    if not pidl:
        return None
    path = ctypes.create_unicode_buffer(32768)
    ok = shell32.SHGetPathFromIDListW(pidl, path)
    ole32.CoTaskMemFree(pidl)
    return path.value if ok and path.value else None


def pick_file(title, filters):
    spec = ''.join(f'{label}\0{pattern}\0' for label, pattern in filters) + '\0'
    spec_buf = (ctypes.c_wchar * len(spec))(*spec)
    file_buf = ctypes.create_unicode_buffer(32768)
    ofn = OPENFILENAMEW()
    ofn.lStructSize = ctypes.sizeof(OPENFILENAMEW)
    ofn.hwndOwner = owner()
    ofn.lpstrFilter = ctypes.addressof(spec_buf)
    ofn.nFilterIndex = 1
    ofn.lpstrFile = ctypes.cast(file_buf, wintypes.LPWSTR)
    ofn.nMaxFile = len(file_buf)
    ofn.lpstrTitle = title
    # OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY
    ofn.Flags = 0x80000 | 0x1000 | 0x800 | 0x8 | 0x4
    if comdlg32.GetOpenFileNameW(ctypes.byref(ofn)):
        return file_buf.value
    err = comdlg32.CommDlgExtendedError()
    if err:
        fail(f'The file dialog failed (error {err:#x}).')
    return None


# Python

def same_file(a, b):
    try:
        return os.path.normcase(str(Path(a).resolve())) == os.path.normcase(str(Path(b).resolve()))
    except OSError:
        return False


def suitable_here():
    if sys.version_info < (3, 11):
        return False
    try:
        import numpy  # noqa: F401
        import PIL  # noqa: F401
    except Exception:
        return False
    return True


def private_env():
    # Pinned to its own folder: without PYTHONHOME the embeddable interpreter adds a registered Python 3.13's Lib and
    # DLLs from the registry ahead of its own; PYTHONNOUSERSITE keeps the player's own packages from shadowing these.
    env = dict(os.environ)
    env['PYTHONHOME'] = str(PRIVATE)
    env.pop('PYTHONPATH', None)
    env['PYTHONNOUSERSITE'] = '1'
    return env


def private_check():
    r = subprocess.run([str(PRIVATE_EXE), '-B', '-s', str(HERE / 'setup.py'), '--check'], env=private_env(),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    return r.returncode == 0, r.stdout.strip()


def download(url, sha, dest):
    name = url.rsplit('/', 1)[1]
    print(f'== python: Downloading {name}', flush=True)
    h = hashlib.sha256()
    req = urllib.request.Request(url, headers={'User-Agent': 'Deckrunner-setup'})
    with urllib.request.urlopen(req, timeout=120) as r, open(dest, 'wb') as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            h.update(chunk)
    if h.hexdigest() != sha:
        os.remove(dest)
        fail(f'{name} does not match its published SHA-256 (got {h.hexdigest()}); nothing was installed.')


def fetch_private(have_interpreter):
    """The interpreter (unless the .cmd already fetched it) and the two wheels, each checked before it is unpacked."""
    shutil.rmtree(STAGE, ignore_errors=True)
    STAGE.mkdir()
    target = PRIVATE if have_interpreter else STAGE / 'py'
    for url, sha in (PINS[1:] if have_interpreter else PINS):
        f = STAGE / url.rsplit('/', 1)[1]
        download(url, sha, f)
        # The interpreter goes to the root, the wheels (plain zips) into site-packages.
        dest = target if f.suffix == '.zip' else target / 'Lib' / 'site-packages'
        with zipfile.ZipFile(f) as z:
            z.extractall(dest)
        os.remove(f)
    if not have_interpreter:
        # Without its ._pth the embeddable interpreter starts like a normal one: the script's folder and PYTHONPATH
        # on sys.path (the donor tools rely on both) and Lib\site-packages from site.
        for p in target.glob('python*._pth'):
            p.unlink()
        shutil.rmtree(PRIVATE, ignore_errors=True)
        shutil.move(str(target), str(PRIVATE))
    shutil.rmtree(STAGE, ignore_errors=True)


def find_python():
    if suitable_here():
        return sys.executable, dict(os.environ)
    running_private = PRIVATE_EXE.exists() and same_file(sys.executable, PRIVATE_EXE)
    if PRIVATE_EXE.exists() and not running_private and private_check()[0]:
        return str(PRIVATE_EXE), private_env()
    if not running_private:
        # The .cmd fetched the private interpreter after the player said yes; anything else asks here.
        print(f'\nSetup needs Python 3.11 or newer with numpy and Pillow; {sys.executable} '
              f'(Python {sys.version.split()[0]}) is not that.')
        print('It can download a private copy into the folder "python" here: about 31 MB from python.org and PyPI,')
        print('each file checked against its published SHA-256. Nothing is installed system-wide.\n')
        if not ask('Download it now?'):
            fail('Setup needs Python. Install Python 3.11+ with numpy and Pillow, or run setup again and let it '
                 'download one.')
    fetch_private(running_private)
    ok, out = private_check()
    if not ok:
        # A file gone right after extraction is antivirus software quarantining it.
        gone = [n for n in ('libffi-8.dll', '_ctypes.pyd', 'vcruntime140.dll', 'python313.dll')
                if not (PRIVATE / n).exists()]
        if gone:
            fail(f'Antivirus software removed {", ".join(gone)} from the downloaded Python; allow it and run setup '
                 f'again. ({out.splitlines()[-1] if out else ""})')
        fail(f'The downloaded Python cannot load its modules: {out}')
    return str(PRIVATE_EXE), private_env()


# Cyberpunk 2077

def is_cyberpunk(p):
    return p and (Path(p) / 'bin' / 'x64' / 'Cyberpunk2077.exe').is_file()


def find_cyberpunk():
    import winreg
    found = []
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as k:
            steam = winreg.QueryValueEx(k, 'SteamPath')[0]
        libs = [steam]
        vdf = Path(steam) / 'steamapps' / 'libraryfolders.vdf'
        if vdf.is_file():
            text = vdf.read_text(encoding='utf-8', errors='replace')
            libs += [m.replace('\\\\', '\\') for m in re.findall(r'"path"\s+"([^"]+)"', text)]
        found += [Path(lib) / 'steamapps' / 'common' / 'Cyberpunk 2077' for lib in libs]
    except OSError:
        pass
    for key in (r'SOFTWARE\WOW6432Node\GOG.com\Games\1423049311', r'SOFTWARE\GOG.com\Games\1423049311'):
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key) as k:
                found.append(Path(winreg.QueryValueEx(k, 'path')[0]))
        except OSError:
            pass
    return next((str(f) for f in found if is_cyberpunk(f)), None)


def main(args):
    print(color('91', '\n  DECKRUNNER  -  Skate 3 in Cyberpunk 2077\n'))
    python, env = find_python()
    say(f'Python: {python}')
    if '--check-python' in args:
        return 0

    cp = find_cyberpunk()
    if cp:
        use = message(f'Install into this Cyberpunk 2077 folder?\n\n{cp}', MB_YESNOCANCEL)
        if use not in (IDYES, IDNO):
            return 1
        if use == IDNO:
            cp = None
    if not cp:
        cp = pick_folder('Choose your Cyberpunk 2077 folder (the one holding bin\\x64\\Cyberpunk2077.exe)')
        if not cp:
            return 1
    if not is_cyberpunk(cp):
        fail(f'{cp} is not a Cyberpunk 2077 folder.')
    say(f'Cyberpunk 2077: {cp}')

    prepare = [python, '-B', '-s', str(HERE / 'prepare.py')]
    if '--uninstall' in args:
        return subprocess.call(prepare + ['--cyberpunk', cp, '--uninstall'], env=env)

    skate = pick_file('Choose your Skate 3 (Xbox 360) disc image, or Cancel to pick an extracted disc folder',
                      [('Disc image (*.iso)', '*.iso'), ('All files (*.*)', '*.*')])
    if not skate:
        skate = pick_folder('Choose your extracted Skate 3 disc folder (the one holding default.xex)')
        if not skate:
            return 1
    say(f'Skate 3: {skate}')
    say("\nSetting up. The first run downloads Deckrunner's setup tools (0.3 MB) and WolvenKit (45 MB), and .NET 10 "
        "(37 MB) if you don't have it;\nall from their official sources, each checked against its published hash. "
        "A rerun picks up where it stopped.\n")
    if subprocess.call(prepare + ['--skate', skate, '--cyberpunk', cp, '--clean'], env=env) != 0:
        fail('Setup stopped (see above). Fix what it says and run setup again: it picks up where it stopped.')
    print(color('92', '\nDone. Start the game, load a save, and press Ctrl+Shift+F10 to skate.'))
    return 0


def check():
    """The private interpreter's own test (run as a child, so it sees the wheels just unpacked)."""
    import numpy
    import PIL
    print(sys.version.split()[0], numpy.__version__, PIL.__version__)
    return 0 if sys.version_info >= (3, 11) else 1


VT = False

if __name__ == '__main__':
    argv = sys.argv[1:]
    if '--probe' in argv:
        sys.exit(0)
    if '--check' in argv:
        sys.exit(check())
    VT = enable_vt()
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass
    try:
        code = main(argv)
    except Stop as e:
        print(color('91', f'\n{e}'))
        code = 1
    except KeyboardInterrupt:
        code = 1
    except Exception:
        traceback.print_exc()
        print(color('91', '\nSetup hit an unexpected error (above). Please include it in a bug report.'))
        code = 1
    if '--check-python' not in argv:
        try:
            input('Press Enter to close')
        except EOFError:
            pass
    sys.exit(code)
