"""Check an AC_AUDIO_DUMP capture (16-bit PCM) without listening.

  python -m astral_chain.audio_report <run folder>   (expects audio.pcm and the AUDIO dump line in the log)

Reports duration against the run's wall time, per-10-s loudness, silent share, clipping and abrupt sample jumps
(discontinuities between consecutive samples larger than half the full scale), and writes audio.wav for listening.
"""
import argparse
from pathlib import Path
import re
import struct
import wave

import numpy as np


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('run', type=Path)
    args = parser.parse_args()
    log = (args.run / 'round-00' / 'trace.log').read_text(encoding='utf-8', errors='replace')
    m = re.search(r'AUDIO dump .* rate=(\d+) channels=(\d+)', log)
    if not m:
        raise SystemExit('no AUDIO dump line in the log')
    rate, channels = int(m.group(1)), int(m.group(2))
    raw = (args.run / 'audio.pcm').read_bytes()
    samples = np.frombuffer(raw[:len(raw) // (2 * channels) * 2 * channels], dtype='<i2').reshape(-1, channels)
    seconds = len(samples) / rate
    notes = (args.run / 'run.txt').read_text(encoding='utf-8-sig', errors='replace')
    start = re.search(r'start (\S+)', notes)
    end = re.search(r'end (\S+)', notes)
    print(f'{rate} Hz, {channels} ch, {seconds:.1f} s of audio')
    if start and end:
        from datetime import datetime
        wall = (datetime.fromisoformat(end.group(1)) - datetime.fromisoformat(start.group(1))).total_seconds()
        print(f'run wall time {wall:.0f} s (audio starts after boot; a large shortfall means underruns/gaps)')
    mono = samples.astype(np.float64).mean(axis=1)
    block = rate * 10
    rms = [float(np.sqrt(np.mean(mono[i:i + block] ** 2))) for i in range(0, len(mono), block)]
    print('RMS per 10 s:', ' '.join(f'{r:.0f}' for r in rms))
    per_second = [float(np.sqrt(np.mean(mono[i:i + rate] ** 2))) for i in range(0, len(mono), rate)]
    silent = sum(1 for r in per_second if r < 30)
    print(f'silent seconds: {silent} of {len(per_second)}')
    print(f'clipped samples: {int(np.sum(np.abs(samples) >= 32767))}')
    jumps = int(np.sum(np.abs(np.diff(samples.astype(np.int32), axis=0)) > 16384))
    print(f'abrupt jumps (> half scale between samples): {jumps}')
    with wave.open(str(args.run / 'audio.wav'), 'wb') as out:
        out.setnchannels(channels)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(samples.tobytes())
    print(f'wrote {args.run / "audio.wav"}')


if __name__ == '__main__':
    main()
