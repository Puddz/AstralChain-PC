"""Read the process fields needed by the native harness; not signature validation."""
from pathlib import Path
import hashlib
import struct
from .containers import FormatError


def _thread_capability(data, base, size, field, minimum):
    offset, length = struct.unpack_from('<II', data, base+field)
    if offset < minimum or offset % 4 or length % 4 or not length or offset+length > size:
        raise FormatError('NPDM kernel-capability extent outside access-control section')
    descriptors = struct.unpack_from('<'+str(length//4)+'I', data, base+offset)
    matches = [word for word in descriptors if word & 15 == 7]
    if len(matches) != 1:
        raise FormatError('NPDM requires one core/priority capability')
    word = matches[0]
    last_priority, first_priority = (word >> 4) & 63, (word >> 10) & 63
    first_core, last_core = (word >> 16) & 255, word >> 24
    if not 0 <= first_core <= last_core <= 3 or not 4 <= first_priority <= last_priority <= 63:
        raise FormatError('Unsupported NPDM core/priority range')
    cores = ((1 << (last_core+1))-1) ^ ((1 << first_core)-1)
    priorities = ((1 << (last_priority+1))-1) ^ ((1 << first_priority)-1)
    return cores, priorities


def read_process_meta(path):
    path = Path(path)
    if not 128 <= path.stat().st_size <= 1024*1024:
        raise FormatError('NPDM size outside limits')
    data = path.read_bytes()
    if data[:4] != b'META':
        raise FormatError('Missing NPDM META header')
    aci, aci_size, acid, acid_size = struct.unpack_from('<4I',data,112)
    if aci < 128 or aci_size < 64 or aci+aci_size > len(data) or acid < 128 or acid_size < 0x240 or acid+acid_size > len(data):
        raise FormatError('NPDM access-control extents outside file')
    if aci < acid+acid_size and acid < aci+aci_size:
        raise FormatError('NPDM access-control sections overlap')
    if data[aci:aci+4] != b'ACI0' or data[acid+512:acid+516] != b'ACID':
        raise FormatError('NPDM access-control magic mismatch')
    stack = struct.unpack_from('<I',data,28)[0]
    if not 4096 <= stack <= 8*1024*1024 or stack % 4096:
        raise FormatError('Unsupported main stack size')
    if data[12] & 15 != 7:
        raise FormatError('Harness requires observed AArch64/39-bit address mode')
    cores, priorities = _thread_capability(data, aci, aci_size, 48, 64)
    allowed_cores, allowed_priorities = _thread_capability(data, acid, acid_size, 0x230, 0x240)
    if cores & ~allowed_cores or priorities & ~allowed_priorities:
        raise FormatError('NPDM requested thread capability exceeds ACID bounds')
    if not (cores & (1 << data[15])) or not (priorities & (1 << data[14])):
        raise FormatError('NPDM main thread outside allowed core/priority range')
    return {'sha256':hashlib.sha256(data).hexdigest(), 'stack_size':stack,
            'core_mask':cores, 'priority_mask':priorities,
            'priority':data[14], 'default_core':data[15], 'mmu_flags':data[12],
            'system_resource_size':struct.unpack_from('<I',data,20)[0],
            'program_id':struct.unpack_from('<Q',data,aci+16)[0],
            'signature_verified':False}
