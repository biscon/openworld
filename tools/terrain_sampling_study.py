#!/usr/bin/env python3
"""Offline convergence study. Requires NumPy and the terrain_probe build target.

Outputs native float32 sample dumps, CSV height profiles, and JSON metrics.
Every run resamples through the current shaders; existing dumps are overwritten.
This intentionally stays outside the routine CTest suite.
"""
import argparse
import csv
import json
import subprocess
from pathlib import Path

import numpy as np


def errors(samples, reference, factor):
    """Compare actual triangle heights and bilinear normals at reference positions."""
    height = reference[:, :, 7]
    u = np.arange(height.shape[0]) / factor
    lo = np.minimum(u.astype(int), samples.shape[0] - 2)
    f = u - lo
    a = samples[lo[:, None], lo[None, :]]
    b = samples[lo[:, None], lo[None, :] + 1]
    c = samples[lo[:, None] + 1, lo[None, :]]
    d = samples[lo[:, None] + 1, lo[None, :] + 1]
    x, z = f[None, :], f[:, None]
    mesh = np.where(x + z <= 1,
                    a[:, :, 7] + x * (b[:, :, 7] - a[:, :, 7]) + z * (c[:, :, 7] - a[:, :, 7]),
                    d[:, :, 7] + (1-x) * (c[:, :, 7] - d[:, :, 7]) + (1-z) * (b[:, :, 7] - d[:, :, 7]))
    x, z = x[:, :, None], z[:, :, None]
    normals = ((1-x)*a[:, :, 4:7] + x*b[:, :, 4:7])*(1-z) + ((1-x)*c[:, :, 4:7] + x*d[:, :, 4:7])*z
    normals /= np.linalg.norm(normals, axis=2)[:, :, None]
    angles = np.degrees(np.arccos(np.clip(np.sum(normals * reference[:, :, 4:7], axis=2), -1, 1)))
    error = np.abs(mesh - height)
    return {
        'heightRMS': float(np.sqrt(np.mean(error**2))),
        'heightMax': float(error.max()),
        'normalMean': float(angles.mean()),
        'normalP99': float(np.percentile(angles, 99)),
        'maxStep': float(max(np.abs(np.diff(samples[:, :, 7], axis=axis)).max() for axis in (0, 1))),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, default=Path('cmake-build-debug/terrain_probe'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--patch', type=int, nargs=2, action='append', metavar=('X', 'Z'),
                        help='16 m chunk coordinates; repeat for multiple patches')
    parser.add_argument('--erosion-wavelength', type=float, default=1024)
    parser.add_argument('--erosion-strength', type=float, default=.22)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    spacings = [0.5, 0.25, 0.125, 0.0625]
    report = {'seed': args.seed, 'wavelength': 2048, 'heightScale': 1200,
              'regionWavelength': 16384, 'erosionWavelength': args.erosion_wavelength, 'erosionStrength': args.erosion_strength,
              'patchExtent': 16, 'referenceSpacing': spacings[-1], 'results': []}
    for px, pz in args.patch or [(165, 165), (166, 164), (164, 166)]:
        for rounding in [0, 0.1, 0.3, 0.6]:
            arrays = []
            for spacing in spacings:
                cells = round(16 / spacing)
                path = args.out / f'{px}_{pz}_{rounding}_{spacing}.bin'
                command = [str(args.probe.resolve()), str(path), str(cells), str(spacing),
                           str(px), str(pz), str(rounding), str(args.seed), '1',
                           '--erosion-wavelength', str(args.erosion_wavelength),
                           '--erosion-strength', str(args.erosion_strength)]
                run = subprocess.run(command, check=True, capture_output=True, text=True)
                report.setdefault('device', run.stdout.splitlines()[0])
                arrays.append(np.fromfile(path, dtype=np.dtype('=f4')).reshape(cells+1, cells+1, 8))
            row = {'patch': [px, pz], 'rounding': rounding, 'levels': []}
            for spacing, samples in zip(spacings, arrays):
                row['levels'].append({'spacing': spacing,
                                      **errors(samples, arrays[-1], round(spacing/spacings[-1]))})
            report['results'].append(row)
            print(json.dumps(row), flush=True)
            # A fixed world-space cross-section at the centre of each patch.
            fine = arrays[-1]
            with (args.out / f'profile_{px}_{pz}_{rounding}.csv').open('w', newline='') as file:
                writer = csv.writer(file)
                writer.writerow(['worldX_m', 'worldZ_m', 'baseHeight_m', 'erodedHeight_m'])
                for x, value in enumerate(fine[len(fine)//2]):
                    writer.writerow([px*16+x*spacings[-1], pz*16+8, float(value[3]), float(value[7])])
    (args.out / 'metrics.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
