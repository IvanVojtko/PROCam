#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ProCam Contributors
# SPDX-License-Identifier: BSD-3-Clause
#
# Trains the small burst denoiser on burstpairs output and compares it with
# the renderer's spatial denoise on held-out bursts.
#
# The network sees a temporal merge of k frames (linear CFA planes, scaled to
# the rendered brightness) plus the expected noise per pixel, and predicts a
# correction. Targets are the merge of all N frames with the share of the
# input's noise it still carries removed (Noise2Noise): the full merge leans
# towards the reference frame, so y = w x + (1 - w) z with z independent of
# x, and z = (y - w x) / (1 - w) is an unbiased target whose noise does not
# correlate with the input. w is measured per level on flat areas.
#
# The noise input also follows the merge's tiles: where it rejected
# alternates (motion, a flickering light) it averaged fewer frames, and the
# app raises the expected noise there (relativeTileNoise in
# src/rawpipelinemath.cpp); burstpairs exports each merge's frame count per
# tile for the same map here. Some training patches have a block of the
# single-frame level pasted in (--augment), as such a tile looks.
#
#   train.py <export folder> [--steps N] [--width C] [--out model.pt]
#   train.py <export folder> --eval model.pt [--crops dir]
#   train.py <export folder> --eval model.pt --export <folder>
import argparse
import json
import math
import random
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

# Held out from training: a spread of ISO and scenes.
HELD_OUT = {
    "ProCam_20261031_220834",  # ISO 1223 indoor
    "ProCam_20261101_084617",  # ISO 100 daylight
    "ProCam_20261101_133348",  # ISO 331
    "ProCam_20261101_193602",  # ISO 7522 night, 16 frames
    "ProCam_20261101_211747",  # ISO 3110
    "ProCam_20261103_083242",  # ISO 1327
    "ProCam_20261104_163504",  # ISO 3201
    "ProCam_20261102_121001",  # ISO 112
    "ProCam_20261108_175129",  # ISO 19200 night, 16 frames, shadows lifted 7.7x
    "ProCam_20261108_195826",  # ISO 19200 night, 16 frames, shadows lifted 8x
    "ProCam_20261111_230845",  # ISO 19200 night, 16 frames, small bright display in a dark room
    "ProCam_20261111_230834",  # the same room, misfocused
    "ProCam_20261111_155321",  # ISO 8160 night, 16 frames, basement
    "ProCam_20261113_214819",  # ISO 1663 portrait
    "ProCam_20261117_201044",  # ISO 791, car tail lights
}
# Frames disagree (focus or subject motion): no usable pairs.
EXCLUDED = {"ProCam_20261104_155116"}

# Colour of each CFA phase (raster order of the 2x2 quad) per CFA index.
CFA_CHANNELS = {0: (0, 1, 1, 2), 1: (1, 0, 2, 1), 2: (1, 2, 0, 1), 3: (2, 1, 1, 0)}


# Tile window of the merge (src/rawpipelinemath.cpp TileTables): 16 pixels,
# stride 8, so each pixel lies in two tiles per axis whose weights sum to 1.
TILE_WINDOW = (0.5 - 0.5 * np.cos(2 * np.pi * (np.arange(16) + 0.5) / 16)).astype(np.float32)


def relative_tile_noise(frames):
    """As relativeTileNoise() in the app: each tile's noise variance
    (1 / effective frames) relative to the median tile, at least 1."""
    v = 1 / np.maximum(1, frames)
    typical = np.partition(v.ravel(), v.size // 2)[v.size // 2]
    return np.maximum(1, v / typical).astype(np.float32)


def tile_map_to_pixels(tiles, y0, x0, h, w):
    """As tileMapToPixels() in the app, for the pixels [y0, y0 + h) x
    [x0, x0 + w) of the planes."""
    ty = np.arange(y0, y0 + h) // 8 + 1
    tx = np.arange(x0, x0 + w) // 8 + 1
    wy = TILE_WINDOW[np.arange(y0, y0 + h) + 8 - ty * 8][:, None]
    wx = TILE_WINDOW[np.arange(x0, x0 + w) + 8 - tx * 8][None, :]
    r0, r1 = tiles[ty - 1], tiles[ty]
    return ((1 - wy) * ((1 - wx) * r0[:, tx - 1] + wx * r0[:, tx])
            + wy * ((1 - wx) * r1[:, tx - 1] + wx * r1[:, tx])).astype(np.float32)


def highpass(a):
    return a[..., 1:-1, 1:-1] * 4 - a[..., :-2, 1:-1] - a[..., 2:, 1:-1] - a[..., 1:-1, :-2] - a[..., 1:-1, 2:]


def noise_estimate(x, scale, offset):
    """Noise of planes x (4, H, W) relative to the single-frame noise model,
    and the resulting mean noise std. The app (src/mldenoise.cpp) computes
    exactly this: on every 4th row and column, the 4-neighbour highpass of
    the samples below its median, against the model's variance there."""
    c = x[:, 1:-1:4, 1:-1:4]
    hp = (4 * c - x[:, 0:-2:4, 1:-1:4] - x[:, 2::4, 1:-1:4][:, : c.shape[1]]
          - x[:, 1:-1:4, 0:-2:4] - x[:, 1:-1:4, 2::4][:, :, : c.shape[2]])
    var = scale[:, None, None] * np.clip(c, 0, None) + offset[:, None, None]
    a = np.abs(hp)
    mask = a < np.median(a)
    ratio = math.sqrt(float((hp[mask].astype(np.float64) ** 2).mean()) / 20 / max(float(var[mask].mean()), 1e-20))
    return ratio, ratio * math.sqrt(max(float(var.mean()), 1e-20))


def shadow_lift(meta):
    """How far the photo's tone mapping lifted its shadows above the metered
    exposure (long_gain / target_gain in the rendered photo's sidecar next
    to the burst frames): up to 3x, up to 8x in night mode."""
    stem = meta["stem"]
    main = Path(meta["reference_sidecar"]).parent / f"{stem}.json"
    try:
        photo = json.loads(main.read_text())
        pipeline = photo.get("raw_pipeline", {})
        return max(1.0, float(pipeline["long_gain"]) / float(pipeline["target_gain"]))
    except (OSError, KeyError, ValueError, ZeroDivisionError):
        return 8.0 if meta.get("night_mode") else 3.0


def build_index(root):
    """Per burst and level: the input's noise share w in the full merge and
    the measured noise relative to the model's single-frame noise."""
    cache = root / "index.json"
    # Bursts exported since are added; delete index.json after re-exporting
    # ones it has.
    index = json.loads(cache.read_text()) if cache.exists() else []
    known = {e["stem"] for e in index}
    count = len(index)
    for meta_path in sorted(root.glob("*/meta.json")):
        meta = json.loads(meta_path.read_text())
        if meta["stem"] in known:
            continue
        folder = meta_path.parent
        full = np.load(folder / "full.npy").astype(np.float32)
        hf = highpass(full)
        flat = np.abs(hf) < np.quantile(np.abs(hf[:, ::4, ::4]), 0.5)
        scale, offset = np.array(meta["noise_scale"]), np.array(meta["noise_offset"])
        levels = []
        for level in meta["levels"]:
            x = np.load(folder / level["file"]).astype(np.float32)
            hx = highpass(x)
            entry = dict(level)
            if level["frames"] < meta["frames"]:
                entry["w"] = float((hx * hf)[flat].sum() / max((hx * hx)[flat].sum(), 1e-12))
            else:
                entry["w"] = 1.0
            entry["noise_ratio"], entry["noise_level"] = noise_estimate(x, scale, offset)
            levels.append(entry)
            del x, hx
        index.append({"stem": meta["stem"], "frames": meta["frames"], "cfa": meta["cfa"], "iso": meta["iso"],
                      "noise_scale": meta["noise_scale"], "noise_offset": meta["noise_offset"],
                      "gain": meta.get("target_gain", 1.0) * meta.get("render_exposure", 1.0),
                      "neutral": meta.get("as_shot_neutral"), "lift": shadow_lift(meta), "levels": levels})
        print(f"indexed {meta['stem']}: " + ", ".join(
            f"k={l['frames']} w={l['w']:.2f} noise x{l['noise_ratio']:.2f} {l['noise_level']:.5f}" for l in levels), flush=True)
    if len(index) != count:
        index.sort(key=lambda e: e["stem"])
        cache.write_text(json.dumps(index, indent=1))
    return index


class Burst:
    def __init__(self, root, entry):
        self.entry = entry
        folder = root / entry["stem"]
        self.full = np.load(folder / "full.npy", mmap_mode="r")
        # Inputs with a target clearly cleaner than them.
        self.levels = [l for l in entry["levels"] if l["frames"] < entry["frames"] and l["w"] < 0.6]
        self.arrays = {l["frames"]: np.load(folder / l["file"], mmap_mode="r") for l in self.levels}
        self.spatial = {l["frames"]: np.load(folder / l["spatial_file"], mmap_mode="r")
                        for l in self.levels if "spatial_file" in l}
        # Effective frames per merge tile, by level (all frames: 0).
        self.tiles = {}
        for l in entry["levels"]:
            if "tiles_file" in l:
                self.tiles[0 if l["frames"] == entry["frames"] else l["frames"]] = np.load(folder / l["tiles_file"])
        self.relative = {k: relative_tile_noise(t) for k, t in self.tiles.items()}
        self.scale = torch.tensor(entry["noise_scale"], dtype=torch.float32).view(4, 1, 1)
        self.offset = torch.tensor(entry["noise_offset"], dtype=torch.float32).view(4, 1, 1)

    def relative_noise(self, frames, y0, x0, h, w, device):
        """The app's per-pixel noise variance factor for a level's merge
        (frames 0: all frames); 1 without tile data."""
        if frames not in self.relative:
            return torch.ones((1, h, w), device=device)
        return torch.from_numpy(tile_map_to_pixels(self.relative[frames], y0, x0, h, w)).to(device).unsqueeze(0)


def noise_sigma(burst, level, x, relative=None):
    """Expected noise std of the level's merge at the (5x5 box smoothed)
    signal, times the tile noise factor `relative` (1, h, w); the app
    computes the same."""
    smooth = F.avg_pool2d(x.unsqueeze(0), 5, 1, 2, count_include_pad=False)[0].clamp(min=0)
    var = (burst.scale.to(x.device) * smooth + burst.offset.to(x.device)).clamp(min=1e-10)
    if relative is not None:
        var = var * relative
    return var.sqrt() * level["noise_ratio"]


def make_pair(burst, level, x, y, relative=None, relative_full=None, target=None):
    """Network input, decorrelated target and loss weight for planes x (the
    level) and y (full merge), all at the rendered brightness. relative and
    relative_full: tile noise factors of the level and the full merge
    (1, h, w); target: one made by the caller (augment)."""
    w = level["w"]
    if target is None:
        target = (y - w * x) / (1 - w)
    sigma = noise_sigma(burst, level, x, relative)
    gain = burst.entry["gain"]
    # Loss as L2 on sqrt(displayed brightness): weight from the clean-ish
    # full merge. The tone mapping lifts the shadows by `lift` (up to 8x at
    # night) and leaves the highlights, so the floor that stops the darkest
    # pixels from dominating sits that much lower: grain there is lifted
    # into view.
    floor = 0.01 / burst.entry.get("lift", 3.0)
    weight = 1.0 / (F.avg_pool2d(y.unsqueeze(0), 9, 1, 4, count_include_pad=False)[0].clamp(min=0) * gain + floor)
    if relative_full is not None:
        # Where the full merge rejected alternates too it leans on the same
        # reference frame as the input: no usable target there.
        weight = weight * (2 - relative_full).clamp(0, 1)
    norm = level["noise_level"] * gain
    return torch.cat([x * gain, sigma * gain]) / norm, target * gain, weight, norm


class Denoiser(nn.Module):
    """Small U-Net on the four CFA planes: 8 inputs (planes + noise), a
    residual correction of the planes out."""

    def __init__(self, width=16):
        super().__init__()
        c0, c1, c2 = width, width * 2, width * 3
        self.enc0 = nn.Sequential(nn.Conv2d(8, c0, 3, padding=1), nn.ReLU(), nn.Conv2d(c0, c0, 3, padding=1), nn.ReLU())
        self.down1 = nn.Conv2d(c0, c1, 2, stride=2)
        self.enc1 = nn.Sequential(nn.ReLU(), nn.Conv2d(c1, c1, 3, padding=1), nn.ReLU())
        self.down2 = nn.Conv2d(c1, c2, 2, stride=2)
        self.mid = nn.Sequential(nn.ReLU(), nn.Conv2d(c2, c2, 3, padding=1), nn.ReLU(),
                                 nn.Conv2d(c2, c2, 3, padding=1), nn.ReLU())
        self.up2 = nn.Conv2d(c2, c1, 1)
        self.dec1 = nn.Sequential(nn.Conv2d(c1 * 2, c1, 3, padding=1), nn.ReLU())
        self.up1 = nn.Conv2d(c1, c0, 1)
        self.dec0 = nn.Sequential(nn.Conv2d(c0 * 2, c0, 3, padding=1), nn.ReLU())
        self.out = nn.Conv2d(c0, 4, 3, padding=1)
        self.width = width
        nn.init.zeros_(self.out.weight)
        nn.init.zeros_(self.out.bias)

    def forward(self, z, norm):
        """z: planes and noise at the rendered brightness, divided by the
        image's mean noise std `norm` (B, 1, 1, 1), so one network serves
        every ISO. Returns the denoised planes (not normalised)."""
        x = z[:, :4] * norm
        e0 = self.enc0(z)
        e1 = self.enc1(self.down1(e0))
        m = self.mid(self.down2(e1))
        d1 = self.dec1(torch.cat([F.interpolate(self.up2(m), scale_factor=2, mode="nearest"), e1], 1))
        d0 = self.dec0(torch.cat([F.interpolate(self.up1(d1), scale_factor=2, mode="nearest"), e0], 1))
        return x + self.out(d0) * norm

    def macs_per_pixel(self):
        """Multiply-adds per plane pixel (each plane pixel is a 2x2 quad)."""
        scale = {"down1": 4, "enc1": 4, "dec1": 4, "up1": 4, "down2": 16, "mid": 16, "up2": 16}
        total = 0
        for name, module in self.named_modules():
            if isinstance(module, nn.Conv2d):
                k = module.kernel_size[0] * module.kernel_size[1]
                total += module.in_channels * module.out_channels * k / scale.get(name.split(".")[0], 1)
        return total


def paste_single_frame(burst, level, x, y, x1, y0, x0, size, device):
    """Input and target of `level` with a random block of tiles taken from
    the single-frame level, as a merge looks where it rejected every
    alternate, and the app's tile noise factor for it."""
    single = next(l for l in burst.levels if l["frames"] == 1)
    tiles = burst.relative[level["frames"]]
    # A block of 1-4 x 1-4 tiles inside the patch.
    th, tw = random.randint(1, 4), random.randint(1, 4)
    ty = random.randint(y0 // 8 + 1, max(y0 // 8 + 1, (y0 + size) // 8 - th))
    tx = random.randint(x0 // 8 + 1, max(x0 // 8 + 1, (x0 + size) // 8 - tw))
    block = np.zeros(tiles.shape, np.float32)
    block[ty:ty + th, tx:tx + tw] = 1
    mask = torch.from_numpy(tile_map_to_pixels(block, y0, x0, size, size)).to(device).unsqueeze(0)
    # Effective frames 1 there: variance the median tile's frame count
    # times that of the typical tile.
    frames = burst.tiles[level["frames"]]
    pasted = np.where(block > 0, float(np.median(frames)), tiles)
    relative = torch.from_numpy(tile_map_to_pixels(pasted, y0, x0, size, size)).to(device).unsqueeze(0)
    wk, w1 = level["w"], single["w"]
    target = (1 - mask) * (y - wk * x) / (1 - wk) + mask * (y - w1 * x1) / (1 - w1)
    return (1 - mask) * x + mask * x1, target, relative


def sample_batch(bursts, batch, size, device, by_frames=False, augment=0.0):
    inputs, targets, weights, norms = [], [], [], []
    while len(inputs) < batch:
        burst = random.choice(bursts)
        # The app denoises merges of all frames: by_frames favours the
        # cleaner levels nearest to that (a level of k frames k times as
        # often).
        level = (random.choices(burst.levels, weights=[l["frames"] for l in burst.levels])[0] if by_frames
                 else random.choice(burst.levels))
        h, w = burst.full.shape[1:]
        y0, x0 = random.randrange(0, h - size), random.randrange(0, w - size)
        x = torch.from_numpy(np.asarray(burst.arrays[level["frames"]][:, y0:y0 + size, x0:x0 + size], dtype=np.float32))
        y = torch.from_numpy(np.asarray(burst.full[:, y0:y0 + size, x0:x0 + size], dtype=np.float32))
        x, y = x.to(device), y.to(device)
        # Motion or misalignment: the patch's local means disagree far beyond
        # the noise. Such pairs would teach blurring; skip them.
        relative = burst.relative_noise(level["frames"], y0, x0, size, size, device)
        relative_full = burst.relative_noise(0, y0, x0, size, size, device)
        sigma = noise_sigma(burst, level, x, relative)
        diff = F.avg_pool2d((x - y).unsqueeze(0), 8)[0].abs()
        expected = F.avg_pool2d(sigma.unsqueeze(0), 8)[0] / 8
        if (diff > 6 * expected + 0.002).float().mean() > 0.02:
            continue
        target = None
        if (augment > 0 and level["frames"] > 1 and level["frames"] in burst.relative and 1 in burst.arrays
                and random.random() < augment):
            x1 = torch.from_numpy(np.asarray(burst.arrays[1][:, y0:y0 + size, x0:x0 + size], dtype=np.float32))
            x, target, relative = paste_single_frame(burst, level, x, y, x1.to(device), y0, x0, size, device)
        inp, target, weight, norm = make_pair(burst, level, x, y, relative, relative_full, target)
        inputs.append(inp)
        targets.append(target)
        weights.append(weight)
        norms.append(norm)
    return (torch.stack(inputs), torch.stack(targets), torch.stack(weights),
            torch.tensor(norms, dtype=torch.float32, device=device).view(-1, 1, 1, 1))


def train(args, index, root, device):
    entries = [e for e in index if e["stem"] not in HELD_OUT | EXCLUDED]
    bursts = [Burst(root, e) for e in entries]
    bursts = [b for b in bursts if b.levels]
    print(f"training on {len(bursts)} bursts", flush=True)
    model = Denoiser(args.width).to(device)
    params = sum(p.numel() for p in model.parameters())
    print(f"model: width {args.width}, {params} parameters, ~{model.macs_per_pixel():.0f} MACs per plane pixel",
          flush=True)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    schedule = torch.optim.lr_scheduler.OneCycleLR(optimizer, max_lr=args.lr, total_steps=args.steps, pct_start=0.05)
    started = time.time()
    running = None
    for step in range(1, args.steps + 1):
        inp, target, weight, norm = sample_batch(bursts, args.batch, args.patch, device, args.by_frames, args.augment)
        out = model(inp, norm)
        loss = ((out - target) ** 2 * weight).mean()
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()
        schedule.step()
        running = loss.item() if running is None else running * 0.98 + loss.item() * 0.02
        if step % 250 == 0 or step == args.steps:
            print(f"step {step}: loss {running:.6g} ({time.time() - started:.0f} s)", flush=True)
    torch.save({"width": args.width, "state": model.state_dict()}, args.out)
    print(f"saved {args.out}", flush=True)
    return model


def preview(planes, cfa, neutral, gain):
    """Half-resolution sRGB-ish preview of linear CFA planes."""
    channels = CFA_CHANNELS[cfa]
    rgb = np.zeros((3,) + planes.shape[1:], np.float32)
    counts = np.zeros(3)
    for p in range(4):
        rgb[channels[p]] += planes[p]
        counts[channels[p]] += 1
    rgb /= counts[:, None, None]
    if neutral and len(neutral) == 3:
        n = np.array([float(v) if not isinstance(v, list) else v[0] / v[1] for v in neutral])
        rgb /= (n / n[1])[:, None, None]
    rgb = np.clip(rgb * gain, 0, 1) ** (1 / 2.2)
    return (rgb.transpose(1, 2, 0) * 255 + 0.5).astype(np.uint8)


@torch.no_grad()
def evaluate(model, index, root, device, crops):
    """Weighted MSE against the decorrelated target, relative to doing
    nothing. The target's own noise adds the same amount to every method, so
    differences between methods are fair even though no clean image exists."""
    model.eval()
    rows = []
    for entry in index:
        if entry["stem"] not in HELD_OUT:
            continue
        burst = Burst(root, entry)
        y = torch.from_numpy(np.asarray(burst.full, dtype=np.float32)).to(device)
        for level in burst.levels:
            k = level["frames"]
            x = torch.from_numpy(np.asarray(burst.arrays[k], dtype=np.float32)).to(device)
            h, w = x.shape[1:]
            inp, target, weight, norm = make_pair(burst, level, x, y, burst.relative_noise(k, 0, 0, h, w, device),
                                                  burst.relative_noise(0, 0, 0, h, w, device))
            gain = entry["gain"]
            out = model(inp.unsqueeze(0), torch.full((1, 1, 1, 1), norm, device=device))[0]
            candidates = {"none": inp[:4] * norm, "model": out}
            if k in burst.spatial:
                candidates["renderer"] = torch.from_numpy(np.asarray(burst.spatial[k], dtype=np.float32)).to(device) * gain
            scores = {name: float(((c - target) ** 2 * weight).mean()) for name, c in candidates.items()}
            rows.append((entry["stem"], entry["iso"], k, entry["frames"], scores))
            if crops is not None:
                crops.mkdir(parents=True, exist_ok=True)
                h, w = x.shape[1:]
                cy, cx = h // 2 - 128, w // 2 - 128
                tiles = []
                for name in ("none", "renderer", "model"):
                    if name in candidates:
                        tiles.append(preview(candidates[name][:, cy:cy + 256, cx:cx + 256].cpu().numpy(),
                                             entry["cfa"], entry["neutral"], 1.0))
                tiles.append(preview((y * gain)[:, cy:cy + 256, cx:cx + 256].cpu().numpy(),
                                     entry["cfa"], entry["neutral"], 1.0))
                strip = np.concatenate(tiles, axis=1)
                try:
                    from PIL import Image
                    Image.fromarray(strip).resize((strip.shape[1] * 2, strip.shape[0] * 2), Image.NEAREST).save(
                        crops / f"{entry['stem']}_k{k}.png")
                except ImportError:
                    np.save(crops / f"{entry['stem']}_k{k}.npy", strip)
    print("held-out: weighted MSE vs decorrelated target (lower is better; 'none' = merge without spatial denoise)")
    print(f"{'burst':24s} {'ISO':>6s} {'k/N':>6s} {'none':>10s} {'renderer':>10s} {'model':>10s}  model vs renderer")
    for stem, iso, k, n, s in rows:
        r = s.get("renderer", float("nan"))
        print(f"{stem:24s} {iso:6.0f} {k:>3d}/{n:<2d} {s['none']:10.4g} {r:10.4g} {s['model']:10.4g}  "
              f"{(s['model'] - r) / r * 100:+6.1f}%")
    return rows


# Order of the convolutions in the weight file (src/mldenoise.cpp).
EXPORT_LAYERS = ["enc0.0", "enc0.2", "down1", "enc1.1", "down2", "mid.1", "mid.3", "up2", "dec1.0", "up1", "dec0.0",
                 "out"]


@torch.no_grad()
def export(model, index, root, device, folder):
    """denoise.bin: "PCDN", version 1, width, then per convolution (in
    EXPORT_LAYERS order) out, in, kh, kw as uint32 and float32 weights
    [out][in][kh][kw] and biases [out], little endian. denoise-test.bin: a
    held-out 64x48 crop: "PCDT", height, width, norm, z (8 x h x w) and the
    expected output (4 x h x w), for tests/mldenoise."""
    import struct
    folder.mkdir(parents=True, exist_ok=True)
    modules = dict(model.named_modules())
    with open(folder / "denoise.bin", "wb") as f:
        f.write(b"PCDN" + struct.pack("<II", 1, model.width))
        for name in EXPORT_LAYERS:
            conv = modules[name]
            w = conv.weight.detach().float().cpu().numpy()
            f.write(struct.pack("<4I", *w.shape))
            f.write(w.astype("<f4").tobytes())
            f.write(conv.bias.detach().float().cpu().numpy().astype("<f4").tobytes())
    entry = next(e for e in index if e["stem"] in HELD_OUT)
    burst = Burst(root, entry)
    level = burst.levels[0]
    h, w, y0, x0 = 48, 64, 700, 900
    x = torch.from_numpy(np.asarray(burst.arrays[level["frames"]][:, y0 - 8:y0 + h + 8, x0 - 8:x0 + w + 8],
                                    dtype=np.float32)).to(device)
    y = torch.from_numpy(np.asarray(burst.full[:, y0 - 8:y0 + h + 8, x0 - 8:x0 + w + 8], dtype=np.float32)).to(device)
    k, size = level["frames"], (h + 16, w + 16)
    inp, _, _, norm = make_pair(burst, level, x, y, burst.relative_noise(k, y0 - 8, x0 - 8, *size, device))
    inp = inp[:, 8:-8, 8:-8].cpu()
    # The expected output in plain float32 on the CPU: CUDA convolutions may
    # use TF32, which is off from the app's float32 reference by ~1e-4.
    import copy
    out = copy.deepcopy(model).cpu()(inp.unsqueeze(0), torch.full((1, 1, 1, 1), norm))[0]
    with open(folder / "denoise-test.bin", "wb") as f:
        f.write(b"PCDT" + struct.pack("<IIf", h, w, norm))
        f.write(inp.float().cpu().numpy().astype("<f4").tobytes())
        f.write(out.float().cpu().numpy().astype("<f4").tobytes())
    print(f"exported {folder / 'denoise.bin'} ({(folder / 'denoise.bin').stat().st_size} bytes) and test vector")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--steps", type=int, default=6000)
    parser.add_argument("--batch", type=int, default=24)
    parser.add_argument("--patch", type=int, default=128)
    parser.add_argument("--width", type=int, default=16)
    parser.add_argument("--lr", type=float, default=2e-3)
    parser.add_argument("--out", type=Path, default=Path("denoiser.pt"))
    parser.add_argument("--eval", type=Path)
    parser.add_argument("--crops", type=Path)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--by-frames", action="store_true",
                        help="sample levels in proportion to their frame count (closer to the app's full merges)")
    parser.add_argument("--augment", type=float, default=0.25,
                        help="share of patches with a block of single-frame tiles pasted in")
    parser.add_argument("--export", type=Path, help="write denoise.bin and denoise-test.bin to this folder")
    args = parser.parse_args()
    random.seed(args.seed)
    torch.manual_seed(args.seed)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    index = build_index(args.root)
    if args.eval:
        saved = torch.load(args.eval, map_location=device)
        model = Denoiser(saved["width"]).to(device)
        model.load_state_dict(saved["state"])
    else:
        model = train(args, index, args.root, device)
    if args.export:
        export(model, index, args.root, device, args.export)
    else:
        evaluate(model, index, args.root, device, args.crops)


if __name__ == "__main__":
    main()
