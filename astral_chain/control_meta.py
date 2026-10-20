"""Bounded legacy NACP language/account/save metadata; no authenticity claim."""
from pathlib import Path
import hashlib
import struct
from .containers import FormatError

LANGUAGES = ('en-US','en-GB','ja','fr','de','es-419','es','it','nl','fr-CA','pt','ru','ko','zh-Hant','zh-Hans','pt-BR')


def read_control_meta(path, preferred='en-GB'):
    if preferred not in LANGUAGES:
        raise FormatError('Unknown preferred language')
    with Path(path).open('rb') as source:
        data = source.read(0x4001)
    if len(data) != 0x4000:
        raise FormatError('NACP must be exactly 16 KiB')
    if data[0x3219]:
        raise FormatError('Unsupported compressed NACP titles')
    if data[0x3026] not in (0, 1):
        raise FormatError('Unsupported NACP account switch lock')
    owner, user, journal, device, device_journal = struct.unpack_from('<Q4q', data, 0x3078)
    if min(user, journal, device, device_journal) < 0:
        raise FormatError('Negative NACP save capacity')
    if user + journal > (1 << 63)-1 or device + device_journal > (1 << 63)-1:
        raise FormatError('NACP combined save capacity exceeds signed 64-bit range')
    mask = struct.unpack_from('<I',data,0x302c)[0]
    if not mask or mask & ~0xffff:
        raise FormatError('Unsupported NACP language flags')
    supported=[]
    for index, code in enumerate(LANGUAGES):
        if mask & (1<<index):
            raw=data[index*0x300:index*0x300+0x200]
            if b'\0' not in raw or not raw[0]:
                raise FormatError('Missing/unterminated supported title')
            try:
                raw.split(b'\0',1)[0].decode('utf-8')
            except UnicodeDecodeError as error:
                raise FormatError('Invalid UTF-8 title') from error
            supported.append(code)
    # Port preference policy: requested locale, then US English, then first supported.
    selected=next(code for code in (preferred,'en-US',*supported) if code in supported)
    return {'sha256':hashlib.sha256(data).hexdigest(),'supported_languages':supported,
            'user_account_switch_lock':data[0x3026],
            'save_owner_id':owner,'user_save_size':user,'user_journal_size':journal,
            'device_save_size':device,'device_journal_size':device_journal,
            'preferred_language':preferred,'selected_language':selected,
            'language_code':int.from_bytes(selected.encode('ascii').ljust(8,b'\0'),'little')}
