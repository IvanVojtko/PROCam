#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ProCam Contributors
# SPDX-License-Identifier: BSD-3-Clause
#
# Sanity check of burstpairs output: for every exported burst, the noise of
# each merge level measured in flat areas against the noise model's
# prediction for that many frames, and how well each level lines up with the
# full merge (a shifted level would show as a large difference on edges).
#
#   checkpairs.py <export folder> [stem...]
import json
import sys
from pathlib import Path

import numpy as np


def flat_noise(plane, signal_lo=0.01, signal_hi=0.5, block=8):
    """Noise std and mean signal of the flattest blocks: the std of the
    difference of horizontal neighbours / sqrt(2) in blocks with little
    structure, so edges do not count as noise."""
    h, w = plane.shape
    h, w = h - h % block, w - w % block
    p = plane[:h, :w].astype(np.float32)
    d = (p[:, 1:] - p[:, :-1])[:, : w - block]
    d = d[:, : (w - block) // block * block]
    blocks_d = d.reshape(h // block, block, -1, block).transpose(0, 2, 1, 3).reshape(h // block, -1, block * block)
    mean = p[:, : d.shape[1]].reshape(h // block, block, -1, block).transpose(0, 2, 1, 3)
    mean = mean.reshape(h // block, -1, block * block).mean(-1)
    std = blocks_d.std(-1) / np.sqrt(2)
    ok = (mean > signal_lo) & (mean < signal_hi)
    if ok.sum() < 50:
        return None, None
    std, mean = std[ok], mean[ok]
    keep = std <= np.quantile(std, 0.2)  # flattest fifth
    return float(np.median(std[keep])), float(np.median(mean[keep]))


def main():
    root = Path(sys.argv[1])
    wanted = set(sys.argv[2:])
    for meta_path in sorted(root.glob("*/meta.json")):
        meta = json.loads(meta_path.read_text())
        if wanted and meta["stem"] not in wanted:
            continue
        scale = np.array(meta["noise_scale"])
        offset = np.array(meta["noise_offset"])
        full = np.load(meta_path.parent / "full.npy", mmap_mode="r")
        print(f"{meta['stem']}: {meta['frames']} frames, ISO {meta['iso']:.0f}, noise {meta['noise_source']}")
        for level in meta["levels"]:
            data = np.load(meta_path.parent / level["file"], mmap_mode="r")
            k = level["frames"]
            # Green phase 1 is a green sample for every CFA order but GRBG/GBRG,
            # where it is 0; use the phase whose mean is highest (green).
            phase = int(np.argmax([float(data[c, ::16, ::16].mean()) for c in range(4)]))
            std, signal = flat_noise(np.asarray(data[phase]))
            if std is None:
                print(f"  k={k:2d}: too few flat areas")
                continue
            predicted = np.sqrt(max(scale[phase] * signal + offset[phase], 0) / k)
            # Edge agreement with the full merge: high-pass difference energy.
            a = np.asarray(data[phase, ::2, ::2], dtype=np.float32)
            b = np.asarray(full[phase, ::2, ::2], dtype=np.float32)
            hp = lambda x: x[1:-1, 1:-1] * 4 - x[:-2, 1:-1] - x[2:, 1:-1] - x[1:-1, :-2] - x[1:-1, 2:]
            edge = np.abs(hp(b)) > np.quantile(np.abs(hp(b)), 0.99)
            mismatch = float(np.abs(hp(a) - hp(b))[edge].mean() / max(np.abs(hp(b))[edge].mean(), 1e-9))
            print(f"  k={k:2d}: flat noise {std:.5f} at {signal:.3f}, model {predicted:.5f} "
                  f"(ratio {std / max(predicted, 1e-9):.2f}), rejection {level['rejection']:.3f}, "
                  f"edge mismatch vs full {mismatch:.2f}")


if __name__ == "__main__":
    main()
