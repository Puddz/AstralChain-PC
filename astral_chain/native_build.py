"""Optional cached Windows object build for generated diagnostic code pages."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess

from .containers import FormatError

FLAGS=['-target','x86_64-windows-gnu','-O1','-mcx16']
SUPPORT='''#include "cpu.h"
extern __thread uint64_t native_executed_steps;  /* per host thread (parallel mode: a shared counter bounced between cores) */
extern const uint64_t native_instruction_limit;
__attribute__((noreturn)) void native_boundary(const char*,Cpu*,uint64_t);
extern unsigned thread_reschedule;
int native_checkpoint(Cpu*,uint64_t);
void native_trace_call(uint64_t,uint64_t);
void native_clear_pair(Cpu*);
void native_load_pair(Cpu*,uint64_t,unsigned,unsigned,unsigned);
uint32_t native_store_pair(Cpu*,uint64_t,unsigned,uint64_t,uint64_t);
#define executed_steps native_executed_steps
#define boundary native_boundary
#define thread_checkpoint native_checkpoint
#define trace_call native_trace_call
#define rt_clear_pair_monitor native_clear_pair
#define rt_load_pair native_load_pair
#define rt_store_pair native_store_pair
'''
BRIDGE='''__thread uint64_t native_executed_steps;
#define executed_steps native_executed_steps
__attribute__((noreturn)) void native_boundary(const char*s,Cpu*c,uint64_t pc) {boundary(s,c,pc);}
int native_checkpoint(Cpu*c,uint64_t step) {return thread_checkpoint(c,step);}
void native_trace_call(uint64_t from,uint64_t to) {trace_call(from,to);}
void native_clear_pair(Cpu*c) {rt_clear_pair_monitor(c);}
void native_load_pair(Cpu*c,uint64_t address,unsigned width,unsigned first,unsigned second) {rt_load_pair(c,address,width,first,second);}
uint32_t native_store_pair(Cpu*c,uint64_t address,unsigned width,uint64_t low,uint64_t high) {return rt_store_pair(c,address,width,low,high);}
'''


def split_dispatch(pages,limit,stop_at=None,chain=False):
    from .native_trace import emit_page,emit_execute
    groups={}
    for page,cases in sorted(pages.items()):
        groups.setdefault(page>>5,[]).append(emit_page(page,cases,limit,stop_at,external=True,chain=chain))
    units={group:SUPPORT+'\n'.join(parts) for group,parts in groups.items()}
    dispatch=BRIDGE+f'\nconst uint64_t native_instruction_limit={limit}ull;\n'+'\n'.join(f'void page_{page:x}(Cpu*);' for page in sorted(pages))
    return units,dispatch+'\n'+emit_execute(pages,stop_at)


def run_compiler(command,timeout=300):
    with subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True) as process:
        try:
            stdout,stderr=process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            subprocess.run(['taskkill','/PID',str(process.pid),'/T','/F'],capture_output=True,timeout=15)
            process.kill();process.communicate(timeout=15)
            raise
        return subprocess.CompletedProcess(command,process.returncode,stdout,stderr)


def digest_file(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()


def build(compiler,reference,units,source,folder,cache,workers=4,extra_objects=()):
    """Cache only complete verified objects. Host runtime is rebuilt every round.

    Callers must not run simultaneous builds against the same cache directory.
    Worker threads own distinct content keys within this build.
    """
    if not 1<=workers<=8:
        raise ValueError('Native build worker count must be 1..8')
    compiler=Path(compiler).resolve();reference=Path(reference).resolve()
    folder=Path(folder).resolve();cache=Path(cache).resolve();cache.mkdir(parents=True,exist_ok=True)
    provenance={'compiler_sha256':digest_file(compiler),'flags':FLAGS,
                'headers':{p.name:digest_file(p) for p in sorted((reference/'runtime').glob('*.h'))}}
    identity=json.dumps(provenance,sort_keys=True).encode()
    def compile_unit(item):
        group,text=item
        key=hashlib.sha256(identity+text.encode()).hexdigest()
        path=cache/key;path.mkdir(exist_ok=True)
        obj=path/'unit.obj';manifest=path/'complete.json'
        if manifest.is_file():
            saved=json.loads(manifest.read_text(encoding='utf-8'))
            if saved.get('key')!=key or not obj.is_file() or saved.get('sha256')!=digest_file(obj):
                raise FormatError('Cached native object integrity mismatch')
            return group,obj,True
        src=path/'unit.c';src.write_text(text,encoding='utf-8')
        result=run_compiler([str(compiler),'cc',*FLAGS,'-I',str(reference/'runtime'),'-c',str(src),'-o',str(obj)])
        (path/'compiler.log').write_text(result.stdout+result.stderr,encoding='utf-8')
        result.check_returncode()
        manifest.write_text(json.dumps({'key':key,'sha256':digest_file(obj),'provenance':provenance},indent=2)+'\n',encoding='utf-8')
        return group,obj,False
    # Distinct groups have distinct emitted page names, hence distinct keys.
    with ThreadPoolExecutor(max_workers=workers) as executor:
        compiled=list(executor.map(compile_unit,sorted(units.items())))
    src=folder/'trace.c';src.write_text(source,encoding='utf-8')
    command=[str(compiler),'cc',*FLAGS,'-I',str(Path(__file__).parent/'runtime'),'-I',str(reference/'runtime'),str(src),
             *(str(obj) for _,obj,_ in compiled),*(str(obj) for obj in extra_objects),'-lbcrypt','-luser32',
             '-Wl,--stack,67108864',  # 64 MB main-thread stack: direct calls nest page functions along guest calls
             '-o',str(folder/'trace.exe')]
    result=run_compiler(command)
    (folder/'compiler.log').write_text(result.stdout+result.stderr,encoding='utf-8')
    stats={'units':len(compiled),'reused':sum(hit for _,_,hit in compiled),'compiled':sum(not hit for _,_,hit in compiled)}
    (folder/'build.json').write_text(json.dumps(stats,indent=2)+'\n',encoding='utf-8')
    result.check_returncode()
    return stats
