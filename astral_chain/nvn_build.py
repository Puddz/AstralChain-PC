"""Cached objects for the vendored LASRecomp NVN layer (runtime/las).

Sources are MIT-licensed LASRecomp files at a recorded commit, plus local shims;
see runtime/las/README.md. Objects are keyed by compiler, flags and every input
file so a stale object is never linked.

Backends:
- null: diagnostic; executes and draws nothing.
- vulkan: LASRecomp's Vulkan backend, presenting into the runtime window. Needs
  the pinned Vulkan-Headers and VulkanMemoryAllocator checkouts under
  third_party/, glslang under tools/ (setup.cmd downloads it), and the system vulkan-1.dll.
"""
import hashlib
import json
from pathlib import Path
import subprocess

from .native_build import FLAGS, digest_file, run_compiler
from .shader_build import prepare_translator

LAS = Path(__file__).parent/'runtime/las'
PROJECT = Path(__file__).resolve().parent.parent
VULKAN_HEADERS = PROJECT/'third_party/Vulkan-Headers/include'
VMA = PROJECT/'third_party/VulkanMemoryAllocator/include'
GLSLANG = PROJECT/'tools/glslang/bin/glslang.exe'
VULKAN_DLL = Path('C:/Windows/System32/vulkan-1.dll')
OBJECT_LAYER = ('nvn/objects.c', 'nvn/cmdbuf.c', 'nvn/formats.c', 'nvn/tegra.c', 'nvn/astc.c')
VULKAN_LAYER = ('nvn/vulkan/device.c', 'nvn/vulkan/exec.c', 'nvn/vulkan/textures.c', 'nvn/vulkan/shaders.c',
                'nvn/vulkan/spirv_cache.c',
                'shaderc_glslang.c', 'game_hooks_neutral.c', 'vma.cpp')
BACKENDS = {'null': ('null_backend.c',), 'vulkan': VULKAN_LAYER}


def nvn_sources(backend):
    if backend not in BACKENDS:
        raise ValueError(f'unknown NVN backend {backend!r}')
    return [LAS/name for name in OBJECT_LAYER+BACKENDS[backend]]


def _pe_exports(path):
    """Named exports of a PE32+ DLL (bounded reads of the export directory)."""
    import struct
    data=path.read_bytes()
    pe=struct.unpack_from('<I',data,0x3c)[0]
    if data[pe:pe+4]!=b'PE\0\0' or struct.unpack_from('<H',data,pe+24)[0]!=0x20b:
        raise ValueError('expected a PE32+ DLL')
    sections=struct.unpack_from('<H',data,pe+6)[0];optional=struct.unpack_from('<H',data,pe+20)[0]
    table=[struct.unpack_from('<8sIIII',data,pe+24+optional+40*i) for i in range(sections)]
    def offset(rva):
        for _,size,address,raw_size,raw in table:
            if address<=rva<address+max(size,raw_size):
                return rva-address+raw
        raise ValueError('export RVA outside sections')
    directory=offset(struct.unpack_from('<I',data,pe+24+112)[0])
    count,names=struct.unpack_from('<I',data,directory+24)[0],struct.unpack_from('<I',data,directory+32)[0]
    result=[]
    for i in range(count):
        start=offset(struct.unpack_from('<I',data,offset(names)+4*i)[0])
        result.append(data[start:data.index(b'\0',start)].decode('ascii'))
    return result


def build_nvn_objects(compiler, reference, cache, backend='null'):
    """Return (objects, link_arguments) for the chosen backend."""
    compiler=Path(compiler).resolve();reference=Path(reference).resolve()
    inputs=sorted(p for p in LAS.rglob('*') if p.is_file() and p.suffix in ('.c','.h','.cpp'))
    provenance={'compiler_sha256':digest_file(compiler),'flags':FLAGS,'backend':backend,
                'inputs':{p.relative_to(LAS).as_posix():digest_file(p) for p in inputs},
                'cpu_h':digest_file(reference/'runtime/cpu.h')}
    if backend=='vulkan':
        provenance['vma_sha256']=digest_file(VMA/'vk_mem_alloc.h')
        provenance['vulkan_core_sha256']=digest_file(VULKAN_HEADERS/'vulkan/vulkan_core.h')
        provenance['vulkan_dll_sha256']=digest_file(VULKAN_DLL)
        provenance['translator_sha256']=digest_file(reference/'runtime/nvn/maxwell/translate.c')
        provenance['translator_patches_sha256']=digest_file(Path(__file__).parent/'shader_build.py')
    key=hashlib.sha256(json.dumps(provenance,sort_keys=True).encode()).hexdigest()
    folder=Path(cache).resolve()/f'nvn-{key}';manifest=folder/'complete.json'
    sources=nvn_sources(backend)
    if backend=='vulkan':
        sources=sources+[folder/'translator/translate-reviewed.c']
    def object_of(source):
        name=source.name if source.is_relative_to(folder) else source.relative_to(LAS).as_posix().replace('/','_')
        return folder/(name+'.obj')
    objects=[object_of(s) for s in sources]
    library=folder/'libvulkan-1.a'
    link=[str(library),'-lc++'] if backend=='vulkan' else []
    if manifest.is_file():
        saved=json.loads(manifest.read_text(encoding='utf-8'))
        expected=objects+([library] if backend=='vulkan' else [])
        if saved.get('key')!=key or not all(o.is_file() and saved['objects'].get(o.name)==digest_file(o) for o in expected):
            raise ValueError('Cached NVN object integrity mismatch')
        return objects,link
    folder.mkdir(parents=True,exist_ok=True)
    common=['-I',str(LAS),'-I',str(reference/'runtime')]
    if backend=='vulkan':
        prepare_translator(reference,folder/'translator')
        common+=['-I',str(LAS/'shim'),'-I',str(VULKAN_HEADERS),'-I',str(VMA),
                 '-I',str(reference/'runtime/nvn/maxwell')]
    log=[]
    for source,obj in zip(sources,objects):
        backend_file=source.is_relative_to(folder) or 'vulkan' in source.parts or source.name in VULKAN_LAYER
        if source.suffix=='.cpp':
            command=[str(compiler),'c++','-target','x86_64-windows-gnu','-O1','-std=c++17',*common,'-c',str(source),'-o',str(obj)]
        else:
            extra=['-include',str(LAS/'las_compat.h')] if backend_file else []
            command=[str(compiler),'cc',*FLAGS,*extra,*common,'-c',str(source),'-o',str(obj)]
        result=run_compiler(command)
        log.append(' '.join(command)+'\n'+result.stdout+result.stderr)
        if result.returncode:
            (folder/'compiler.log').write_text('\n'.join(log),encoding='utf-8')
            result.check_returncode()
    if backend=='vulkan':
        definition=folder/'vulkan-1.def'
        definition.write_text('LIBRARY vulkan-1.dll\nEXPORTS\n'+''.join(n+'\n' for n in _pe_exports(VULKAN_DLL)),encoding='ascii')
        subprocess.run([str(compiler),'dlltool','-m','i386:x86-64','-d',str(definition),'-l',str(library)],check=True,timeout=120)
    (folder/'compiler.log').write_text('\n'.join(log),encoding='utf-8')
    recorded=objects+([library] if backend=='vulkan' else [])
    manifest.write_text(json.dumps({'key':key,'objects':{o.name:digest_file(o) for o in recorded},
                                    'provenance':provenance},indent=2)+'\n',encoding='utf-8')
    return objects,link
