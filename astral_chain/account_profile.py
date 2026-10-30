"""Persistent local PC account identity, separate from game and console data."""
import json
import os
from pathlib import Path
import re
import tempfile
import time
import uuid
from .containers import FormatError


def validate_profile(value):
    if not isinstance(value, dict) or set(value) != {'version', 'uid', 'nickname', 'created_at'}:
        raise FormatError('Invalid local account profile fields')
    if type(value['version']) is not int or value['version'] != 1:
        raise FormatError('Unsupported local account profile version')
    uid = value['uid']
    if not isinstance(uid, str) or not re.fullmatch('[0-9a-f]{32}', uid) or int(uid, 16) == 0:
        raise FormatError('Invalid local account UID')
    name = value['nickname']
    if not isinstance(name, str) or not name or any(ord(c) < 32 or ord(c) == 127 for c in name):
        raise FormatError('Invalid local account nickname')
    try:
        encoded = name.encode('utf-8')
    except UnicodeError as error:
        raise FormatError('Invalid local account nickname encoding') from error
    if len(encoded) > 31:
        raise FormatError('Local account nickname exceeds 31 UTF-8 bytes')
    if type(value['created_at']) is not int or not 0 <= value['created_at'] < 1 << 63:
        raise FormatError('Invalid local account creation timestamp')
    return value


def _unique_fields(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise FormatError('Duplicate local account profile field')
        result[key] = value
    return result


def load_profile(path):
    """Create once with atomic no-overwrite publication; never replace bad data."""
    path = Path(path)
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        value = validate_profile(dict(version=1, uid=uuid.uuid4().hex, nickname='Player', created_at=int(time.time())))
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent,
                                             prefix='.account-', suffix='.tmp', delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(json.dumps(value, ensure_ascii=False, indent=2)+'\n')
                stream.flush()
                os.fsync(stream.fileno())
            try:
                os.link(temporary, path)
            except FileExistsError:
                pass  # Another creator published its complete profile first.
            except OSError:
                # Filesystems without hard links (exFAT, FAT32, some network drives):
                # fall back to exclusive creation, which still never overwrites.
                try:
                    with open(path, 'xb') as stream:
                        stream.write(temporary.read_bytes())
                        stream.flush()
                        os.fsync(stream.fileno())
                except FileExistsError:
                    pass
        finally:
            if temporary is not None:
                temporary.unlink()
    with path.open('rb') as stream:
        raw = stream.read(4097)
    if len(raw) > 4096:
        raise FormatError('Local account profile exceeds 4096 bytes')
    try:
        value = json.loads(raw.decode('utf-8'), object_pairs_hook=_unique_fields)
    except (ValueError, UnicodeError) as error:
        raise FormatError('Malformed local account profile JSON') from error
    return validate_profile(value)


def account_runtime_constants(profile):
    validate_profile(profile)
    uid = ','.join(f'0x{byte:02x}' for byte in bytes.fromhex(profile['uid']))
    return '#define ACCOUNT_UID_BYTES {'+uid+'}\n'
