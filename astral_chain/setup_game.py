"""One-time setup: build the native Windows game from your own Astral Chain dump.

  python -m astral_chain.setup_game [--base FILE] [--update FILE] [--keys prod.keys]

Stages (each is skipped when its result already exists, so a failed run can be fixed and repeated):

  1. tools    download Zig, glslang and NSTool at pinned versions (SHA-256 checked) into tools/
  2. base     extract the base game's Program and Control NCAs (XCI or NSP) with your keys
  3. update   extract the v1.0.1 update's ExeFS, and its RomFS merged over the base
  4. romfs    pack the merged RomFS into game/romfs.bin, then delete the extracted copies
  5. build    translate the game code to C and compile build/game/round-00/trace.exe

Nothing here leaves your PC. Keys are read in place by NSTool and never copied; NSPs carry their own tickets. Files picked through the dialogs
are remembered in userdata/setup.json (paths only) for the next run.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

PROJECT = Path(__file__).resolve().parent.parent
TOOLS = PROJECT/'tools'
GAME = PROJECT/'game'
WORK = GAME/'work'
BUILD = PROJECT/'build'
USERDATA = PROJECT/'userdata'
SETTINGS = USERDATA/'setup.json'

DOWNLOADS = {
    'zig': ('https://ziglang.org/download/0.17.0/zig-x86_64-windows-0.17.0.zip',
            'b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428', 'zig-x86_64-windows-0.17.0/'),
    'glslang': ('https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip',
                '82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b', ''),
    'nstool': ('https://github.com/jakcron/nstool/releases/download/v1.9.2/nstool-v1.9.2-win_x64.zip',
               '34659dc47249a3c00f7fc88a11f4e5ba14dfbec0a6a96b4f3f71368a7e1c9374', ''),
}
ZIG = TOOLS/'zig'/'zig.exe'
NSTOOL = TOOLS/'nstool'/'nstool.exe'
GLSLANG = TOOLS/'glslang'/'bin'/'glslang.exe'
ROMFS_TOOL = TOOLS/'romfs-build.exe'

LASRECOMP = PROJECT/'third_party'/'LASRecomp'
PATCHES = (PROJECT/'third_party'/'AstralChain-Dynamic-FPS'/'1.0.1.pchtxt',
           PROJECT/'astral_chain'/'patches'/'cutscene-catchup-1.0.1.pchtxt')
MAIN_SHA_101 = 'dc47cae835866379c597e1c507bf52ee0f865ec4412930dfdf1bbc20b6bb9c3b'
PROGRAM_ID = '01007300020fa000'
FREE_SPACE_GB = 35  # extracted RomFS (~10 GB) + its image (~10 GB) + NCAs + build, before the cleanup

UPDATE_EXEFS = GAME/'update-exefs'
CONTROL = GAME/'control.nacp'
ROMFS_IMAGE = GAME/'romfs.bin'
GAME_BUILD = BUILD/'game'
FINGERPRINT = GAME_BUILD/'build-fingerprint.txt'


class SetupError(Exception):
    pass


def step(text):
    print(f'\n== {text}', flush=True)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


# ---------------------------------------------------------------------------------------------------- 1. tools
def fetch_tools():
    for name, (url, digest, prefix) in DOWNLOADS.items():
        target = TOOLS/name
        if target.is_dir():
            continue
        step(f'Downloading {name}')
        TOOLS.mkdir(exist_ok=True)
        archive = TOOLS/f'{name}.zip'
        with urllib.request.urlopen(url, timeout=120) as response, open(archive, 'wb') as out:
            shutil.copyfileobj(response, out)
        if sha256(archive) != digest:
            archive.unlink()
            raise SetupError(f'{name}: the download does not match its pinned SHA-256 ({url})')
        partial = TOOLS/f'{name}.partial'
        shutil.rmtree(partial, ignore_errors=True)
        with zipfile.ZipFile(archive) as z:
            for member in z.infolist():
                if not member.filename.startswith(prefix) or member.is_dir():
                    continue
                relative = member.filename[len(prefix):]
                destination = (partial/relative).resolve()
                if partial.resolve() not in destination.parents:
                    raise SetupError(f'{name}: unexpected path in the archive: {member.filename}')
                destination.parent.mkdir(parents=True, exist_ok=True)
                with z.open(member) as source, open(destination, 'wb') as out:
                    shutil.copyfileobj(source, out)
        partial.rename(target)
        archive.unlink()
    if not ROMFS_TOOL.is_file():
        step('Building the RomFS packer')
        run([ZIG, 'cc', '-O2', '-target', 'x86_64-windows-gnu', '-I', LASRECOMP/'runtime',
             PROJECT/'astral_chain'/'runtime'/'romfs_tool.c', LASRECOMP/'runtime'/'romfs_build.c', '-o', ROMFS_TOOL])


def run(command, **kwargs):
    command = [str(c) for c in command]
    result = subprocess.run(command, capture_output=True, text=True, errors='replace', **kwargs)
    if result.returncode:
        tail = '\n'.join((result.stdout + result.stderr).strip().splitlines()[-15:])
        raise SetupError(f'{Path(command[0]).name} failed ({result.returncode}):\n{tail}')
    return result.stdout + result.stderr


# ---------------------------------------------------------------------------------------------------- inputs
def pick_inputs(args):
    saved = json.loads(SETTINGS.read_text(encoding='utf-8')) if SETTINGS.is_file() else {}
    wanted = [('keys', 'your prod.keys', [('Keys', '*.keys'), ('All files', '*.*')]),
              ('base', 'the Astral Chain base game (XCI or NSP)', [('Switch games', '*.xci *.nsp'), ('All files', '*.*')]),
              ('update', 'the Astral Chain v1.0.1 update (NSP)', [('Switch updates', '*.nsp'), ('All files', '*.*')])]
    values = {}
    for key, label, types in wanted:
        given = getattr(args, key) or saved.get(key)
        if given and Path(given).is_file():
            values[key] = Path(given)
            continue
        print(f'Select {label}...', flush=True)
        values[key] = ask_file(label, types)
    USERDATA.mkdir(exist_ok=True)
    SETTINGS.write_text(json.dumps({k: str(v) for k, v in values.items() if v}, indent=2) + '\n', encoding='utf-8')
    return values


def ask_file(label, types):
    try:
        import tkinter
        from tkinter import filedialog
        root = tkinter.Tk()
        root.withdraw()
        root.attributes('-topmost', True)
        chosen = filedialog.askopenfilename(title=f'Astral Chain setup: select {label}', filetypes=types)
        root.destroy()
    except Exception:
        chosen = input(f'Path to {label}: ').strip().strip('"')
    if not chosen or not Path(chosen).is_file():
        raise SetupError(f'No file selected for {label}.')
    return Path(chosen)


# ---------------------------------------------------------------------------------------------------- 2/3. NCAs
def unpack(container, folder, inputs):
    """The NCAs (and ticket) of an XCI's secure partition or an NSP, into folder."""
    if folder.is_dir() and any(folder.glob('*.nca')):
        return
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    with open(container, 'rb') as stream:
        head = stream.read(0x110)
    if head[:4] == b'PFS0':
        source = '/'
    elif head[0x100:0x104] == b'HEAD':
        source = '/secure/'
    else:
        raise SetupError(f'{container.name} is neither an NSP nor an XCI.')
    run([NSTOOL, '-k', inputs['keys'], '-x', source, str(folder) + os.sep, container])
    if not any(folder.glob('*.nca')):
        raise SetupError(f'No NCAs found in {container.name}.')


def nca_kind(nca, inputs, tik):
    info = run([NSTOOL, '-k', inputs['keys'], *(['--tik', tik] if tik else []), nca])
    kind = program = None
    for line in info.splitlines():
        line = line.strip()
        if line.startswith('Content Type:'):
            kind = line.split(':', 1)[1].strip()
        elif line.startswith('ProgID:'):
            program = line.split(':', 1)[1].strip().lower().removeprefix('0x')
    return kind, program


def find_ncas(folder, inputs):
    tik = next(iter(sorted(folder.glob('*.tik'))), None)
    found = {}
    for nca in sorted(folder.glob('*.nca')):
        if nca.name.endswith('.cnmt.nca'):
            continue
        kind, program = nca_kind(nca, inputs, tik)
        if program and program != PROGRAM_ID:
            raise SetupError(f'{nca.name} belongs to program {program}, not Astral Chain ({PROGRAM_ID}).')
        if kind in ('Program', 'Control'):
            found[kind] = nca
    return found, tik


def extract_base(inputs):
    folder = WORK/'base'
    unpack(inputs['base'], folder, inputs)
    ncas, tik = find_ncas(folder, inputs)
    if 'Program' not in ncas or 'Control' not in ncas:
        raise SetupError('The base game file has no Astral Chain Program/Control NCA (is it the update instead?).')
    if not CONTROL.is_file():
        step('Extracting the game metadata')
        control = WORK/'control'
        shutil.rmtree(control, ignore_errors=True)
        run([NSTOOL, '-k', inputs['keys'], *(['--tik', tik] if tik else []), '--part0', str(control), ncas['Control']])
        if not (control/'control.nacp').is_file():
            raise SetupError('control.nacp was not found in the Control NCA.')
        shutil.copyfile(control/'control.nacp', CONTROL)
    return ncas['Program'], tik


def extract_update(inputs, base_program, base_tik):
    folder = WORK/'update'
    unpack(inputs['update'], folder, inputs)
    ncas, tik = find_ncas(folder, inputs)
    if 'Program' not in ncas:
        raise SetupError('The update file has no Astral Chain Program NCA.')
    if not (UPDATE_EXEFS/'main').is_file():
        step('Extracting the update code (ExeFS)')
        shutil.rmtree(UPDATE_EXEFS, ignore_errors=True)
        run([NSTOOL, '-k', inputs['keys'], *(['--tik', tik] if tik else []), '--part0', str(UPDATE_EXEFS), ncas['Program']])
    if sha256(UPDATE_EXEFS/'main') != MAIN_SHA_101:
        shutil.rmtree(UPDATE_EXEFS, ignore_errors=True)
        raise SetupError('The update is not Astral Chain v1.0.1 (its main module does not match). '
                         'This port needs exactly v1.0.1.')
    return ncas['Program'], tik


def build_romfs(inputs, base_program, update_program, update_tik):
    if ROMFS_IMAGE.is_file():
        return
    tree = WORK/'romfs'
    step('Extracting the game data (RomFS, about 10 GB; this takes a while)')
    shutil.rmtree(tree, ignore_errors=True)
    run([NSTOOL, '-k', inputs['keys'], *(['--tik', update_tik] if update_tik else []), '--basenca', base_program,
         '--part1', str(tree), update_program])
    step('Packing the game data into game/romfs.bin')
    partial = GAME/'romfs.bin.partial'
    run([ROMFS_TOOL, tree, partial])
    partial.rename(ROMFS_IMAGE)


# ---------------------------------------------------------------------------------------------------- 5. build
def fingerprint():
    """Changes to the sources, vendored code or patches make the next launch rebuild."""
    digest = hashlib.sha256()
    for root in (PROJECT/'astral_chain', PROJECT/'third_party'):
        for path in sorted(root.rglob('*')):
            if path.is_file() and '__pycache__' not in path.parts:
                digest.update(str(path.relative_to(PROJECT)).encode())
                digest.update(path.read_bytes())
    return digest.hexdigest()


def build_is_current():
    return (GAME_BUILD/'round-00'/'trace.exe').is_file() and FINGERPRINT.is_file() and \
        FINGERPRINT.read_text(encoding='ascii').strip() == fingerprint()


def build_game():
    """native_trace wants a new folder: an older build moves to build/game-old, its shader cache is carried over."""
    if build_is_current():
        return
    step('Building the game (translating and compiling; the first build takes several minutes)')
    old = BUILD/'game-old'
    if GAME_BUILD.exists():
        shutil.rmtree(old, ignore_errors=True)
        GAME_BUILD.rename(old)
    command = [sys.executable, '-m', 'astral_chain.native_trace', UPDATE_EXEFS,
               '--reference', LASRECOMP, '--compiler', ZIG, '--output', GAME_BUILD,
               '--instructions', '4000000', '--sample-steps', '0', '--run-timeout', '86400', '--unbounded', '--chain',
               '--follow', '0', '--entry', 'rtld:start', '--seed-all-text', '--build', 'cached',
               '--control', CONTROL, '--romfs', ROMFS_IMAGE, '--nvn-hle', '--nvn-backend', 'vulkan',
               '--optimize', '--lean', '--direct-calls', '--jumps']
    for patch in PATCHES:
        command += ['--patch', patch]
    command.append('--build-only')
    result = subprocess.run([str(c) for c in command], cwd=PROJECT)
    if result.returncode or not (GAME_BUILD/'round-00'/'trace.exe').is_file():
        raise SetupError('The game build failed (see the messages above).')
    FINGERPRINT.write_text(fingerprint() + '\n', encoding='ascii')
    if (old/'cache').is_dir() and not (GAME_BUILD/'cache').exists():
        (old/'cache').rename(GAME_BUILD/'cache')


def game_data_ready():
    return (UPDATE_EXEFS/'main').is_file() and CONTROL.is_file() and ROMFS_IMAGE.is_file()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--base', type=Path, help='base game XCI or NSP')
    parser.add_argument('--update', type=Path, help='v1.0.1 update NSP')
    parser.add_argument('--keys', type=Path, help='prod.keys')
    parser.add_argument('--keep-temp', action='store_true', help='keep the extracted NCAs and RomFS folder')
    parser.add_argument('--rebuild-if-stale', action='store_true', help='(launcher) rebuild only if sources changed')
    args = parser.parse_args()
    try:
        if args.rebuild_if_stale:
            if not game_data_ready():
                raise SetupError('The game is not set up yet: run setup.cmd first.')
            if not build_is_current():
                fetch_tools()
                build_game()
            return
        GAME.mkdir(exist_ok=True)
        if not game_data_ready():
            free = shutil.disk_usage(PROJECT).free / 1e9
            if free < FREE_SPACE_GB:
                raise SetupError(f'Setup needs about {FREE_SPACE_GB} GB free on this drive ({free:.0f} GB free).')
        fetch_tools()
        if not game_data_ready():
            inputs = pick_inputs(args)
            step('Reading the base game')
            base_program, base_tik = extract_base(inputs)
            step('Reading the update')
            update_program, update_tik = extract_update(inputs, base_program, base_tik)
            build_romfs(inputs, base_program, update_program, update_tik)
        if not args.keep_temp and WORK.exists():
            step('Removing temporary files')
            shutil.rmtree(WORK, ignore_errors=True)
        build_game()
        print('\nSetup complete. Start the game with "Play Astral Chain.cmd".')
    except SetupError as error:
        print(f'\nSetup stopped: {error}', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
