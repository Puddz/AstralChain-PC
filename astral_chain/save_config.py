"""Identity and paths for native PC saves, independent of trace output folders."""
from pathlib import Path
import struct
from .account_profile import validate_profile
from .containers import FormatError


def save_configuration(process, control, profile, profile_path):
    validate_profile(profile)
    if control is None:
        return None
    application_id = process['program_id'] & ~15
    if not application_id or control['save_owner_id'] != application_id:
        raise FormatError('Unsupported save owner: expected the application program ID')
    if control['device_save_size'] or control['device_journal_size']:
        raise FormatError('Device save provisioning is not implemented')
    if not control['user_save_size']:
        raise FormatError('Application has no supported user save capacity')
    root = Path(profile_path).resolve().parent/'saves'/f'{application_id:016x}'/profile['uid']
    identity = struct.pack('<8sQQ16sQQQ', b'ACPCSAV1', application_id,
                           control['save_owner_id'], bytes.fromhex(profile['uid']),
                           control['user_save_size'], control['user_journal_size'], 0)
    return {'root':str(root), 'identity':identity.hex(), 'application_id':application_id,
            'data_capacity':control['user_save_size'], 'journal_capacity':control['user_journal_size']}


def save_runtime_constants(config):
    if config is None:
        return ''
    # Numeric UTF-16 initializer avoids C string escaping and non-ASCII source encodings.
    encoded=config['root'].encode('utf-16-le')
    units=struct.unpack('<'+str(len(encoded)//2)+'H',encoded)
    root=','.join(str(unit) for unit in (*units,0))
    identity=','.join(str(byte) for byte in bytes.fromhex(config['identity']))
    return '#define SAVE_ROOT_UNITS {'+root+'}\n#define SAVE_IDENTITY_BYTES {'+identity+'}\n'
