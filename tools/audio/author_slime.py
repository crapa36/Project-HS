"""Author four distinct slime variations, preserving the original cue variation count.

Procedural elastic FM resonance and filtered noise. Stable Audio's gated model
returned HTTP 401, so this asset uses the explicitly approved procedural route.
Existing candidates and finalized assets are never overwritten.
"""
import argparse
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
import wave

ROOT = Path(__file__).resolve().parents[2]
# pitch ratio, resonance modulation, filtered-noise mix, duration, decay rate
VARIANTS = {
    'windup': [(1.0, .9, .10, .35, 1.3), (.89, .74, .13, .35, 1.15),
               (1.12, 1.02, .08, .35, 1.45), (.96, 1.12, .15, .35, 1.25)],
    'hit': [(1.0, .9, .20, .26, 17), (.86, 1.05, .24, .28, 15),
            (1.15, .72, .16, .23, 20), (.95, 1.18, .22, .27, 18)],
}

def create(kind, version):
    pitch, resonance, wet, duration, decay = VARIANTS[kind][version - 1]
    cue = 'slime_melee_' + kind + '_sfx'
    out = ROOT / 'GeneratedAudio' / cue
    out.mkdir(parents=True, exist_ok=True)
    path = out / f'v{version:02d}.wav'
    destination = ROOT / 'ContentSource/Audio' / f'{cue}_v{version:02d}.wav'
    if path.exists() or path.with_suffix('.json').exists() or destination.exists():
        raise RuntimeError(f'Refusing to overwrite {path} or {destination}')
    seed = (913 if kind == 'windup' else 914) + (version - 1) * 101
    rng = random.Random(seed)
    phase = low = 0.0
    samples = []
    sr = 48000
    for n in range(round(duration * sr)):
        t = n / sr
        x = t / duration
        if kind == 'windup':
            hz = pitch * (150 + 240*x*x + 24*math.sin(x*math.pi*5))
            env = math.sin(math.pi*x)**decay
        else:
            hz = pitch * (80 + 230*math.exp(-t*27))
            env = (1-math.exp(-t*500))*math.exp(-t*decay)*(1-x)**2
        phase += 2*math.pi*hz/sr
        low = .88*low + .12*rng.uniform(-1, 1)
        value = env * (.34*math.sin(phase + resonance*math.sin(phase*.51))
                       + .10*math.sin(phase*2.03) + wet*low)
        samples.append(max(-32767, min(32767, round(value*32767))))
    with wave.open(str(path), 'wb') as wav:
        wav.setparams((1, 2, sr, 0, 'NONE', 'not compressed'))
        wav.writeframes(struct.pack('<' + 'h'*len(samples), *samples))
    path.with_suffix('.json').write_text(json.dumps({
        'cue_id': cue, 'generator': 'procedural FM elastic resonance and filtered noise',
        'seed': seed, 'duration_seconds': duration, 'pitch_ratio': pitch,
        'resonance_modulation': resonance, 'noise_mix': wet, 'envelope_decay': decay,
        'reason': 'Stable Audio 3 small-sfx model access failed HTTP 401; procedural route authorized',
        'listening_acceptance': 'pending user listening',
    }, indent=2) + '\n', encoding='utf-8')
    subprocess.run([sys.executable, str(ROOT/'tools/audio/finalize.py'),
                    str(path), str(destination), '--kind', 'spatial-sfx'], check=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--versions', type=int, choices=range(1, 5), nargs='+', default=[1, 2, 3, 4])
    args = parser.parse_args()
    for kind in VARIANTS:
        for version in args.versions:
            create(kind, version)
