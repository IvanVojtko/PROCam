<!--
SPDX-FileCopyrightText: 2026 RAWfish Contributors
SPDX-License-Identifier: BSD-3-Clause
-->

# Lens shading calibration

Files here correct per-camera vignetting (corner darkening) and the
associated color shift (red/blue falloff faster or slower than green) when
saving RAW captures as DNG. The correction is written into the DNG itself as
a standard `OpcodeList2` / `GainMap` opcode (see DNG spec >= 1.3), so it is
applied automatically by any DNG-aware raw processor (Lightroom, darktable,
RawTherapee, etc.) -- ProCam does not need to do any pixel processing
itself.

## How it works

* `lens_shading_<model-slug>_camera<ID>_<width>x<height>_<ratio>.json` holds
  a coarse per-CFA-channel (R, Gr, Gb, B) gain grid, computed from photos of
  a flat, evenly lit, neutral grey/white target, for one specific phone
  model + Camera2 camera id + RAW resolution. All four are baked into the
  file name (`model-slug` is the phone's model string, lowercased and
  stripped to `[a-z0-9-]`; `ratio` is the GCD-reduced aspect ratio, e.g.
  `4x3`), because a different resolution can be a different sensor
  crop/binning mode with genuinely different vignetting, not just a resize
  -- see "Limitations" below.
* `DngLensShading::buildOpcodeList2()` (`src/dnglensshading.cpp`) turns that
  grid into the DNG `GainMap` opcodes and `writeTiffDng()`
  (`src/declarativecameraextensions.cpp`) attaches them to every DNG saved,
  by building that exact file name from the capture's own device
  model/camera id/resolution and looking it up -- first in
  the `lens-shading/` subdirectory of ProCam's device-profiles directory
  (normally `~/.local/share/com.procam/procam/device-profiles/`; a
  user-writable override, no rebuild/repackage needed), then in this
  directory as bundled with the app.
* If neither directory has a matching file, or the one found does not
  match the capture's CFA pattern (checked again, independently of the
  file name, as a second guard), no opcode is written and the DNG is
  saved without lens shading correction.

## Regenerating a calibration

1. Mount/point the camera at a flat, evenly lit, neutral grey or white
   card filling the entire frame (out of focus is fine and often better,
   since it hides texture in the card). Avoid mixed lighting and keep the
   card as flat as possible -- an unevenly lit card is the most common
   cause of a lopsided result (see "Diagnostics" below).
2. Capture one or more RAW DNGs of the card with ProCam
   (in the settings, choose DNG or RAW + JSON + DNG as the RAW format).
3. Run the generator -- no flags needed beyond the DNGs themselves; device
   model, camera id, resolution and CFA are all read straight from the
   DNGs' own tags (`Model`/`UniqueCameraModel`, both written by ProCam
   itself), and the output file name is derived from those same values:

   ```
   python3 tools/calibration/generate_lens_shading.py flat1.dng flat2.dng
   ```

   Passing more than one capture reduces sensor noise in the calibration and
   the tool reports how much the independent estimates disagreed, as a
   sanity check. All input DNGs must be from the same device model, camera
   id, resolution and CFA pattern -- the tool refuses to mix them.
4. Take a new DNG with that camera and confirm the `OpcodeList2` tag is now
   present (e.g. `exiftool -OpcodeList2 capture.dng`) and that a DNG-aware
   viewer shows flatter corners and less color drift than before. Or run
   `tools/calibration/verify_gain_map.py` (see below) for a quantitative
   check that doesn't depend on eyeballing a viewer.
5. Copy the generated file into either:
   - `~/.local/share/com.procam/procam/device-profiles/lens-shading/` on
     the device, to use it immediately without rebuilding or repackaging
     ProCam (ProCam creates this directory itself the first time it
     saves a DNG, if it doesn't exist yet); or
   - this directory in the source tree, to bundle it with the app -- e.g.
     to contribute it upstream.

   At the moment only camera 0 of the Jolla Phone (2026), at 4096x3072, is
   calibrated and bundled. Any other camera or RAW resolution needs its own
   calibration file; without one, the DNG is saved without correction.

## Diagnostics

The generator prints two checks after averaging, before writing the file:

* **Clamping report**: for each plane, how many of its grid points hit
  `--max-gain` (see below), split into the leftmost vs. rightmost quarter
  of grid columns. A result skewed heavily to one side (more than double
  the other) almost always means the flat-field target itself was lit
  unevenly, not that `--max-gain` is too low -- retake the flat with more
  even light first. A real, symmetric lens falloff clamps roughly evenly
  on both edges.
* **Dosing summary**: the pivot gain value `--balance`/`--strength`
  settled on (see below), and whether the frame centre will be darkened
  as a result.

## Tuning the correction: --max-gain, --balance, --strength

* `--max-gain` (default 4.0) caps the gain applied at any single grid
  point, to avoid amplifying sensor noise in very dark corners. Raise it
  only after checking the clamping report above shows a genuinely
  symmetric (not lopsided) clamp; otherwise fix the flat-field capture
  instead.
* `--balance` (0-100, default 0) trades brightening the corners for
  darkening the centre instead. By default (0) every correction only ever
  brightens a pixel up towards the least-vignetted part of the frame, so
  the centre is never touched. At 100, the single point needing the *most* correction
  (usually a far corner) is left unchanged and everything else, including
  the centre, is only ever darkened down towards it. Values in between
  blend the two, letting you keep the overall image from getting brighter
  when the corners need a lot of correction.
* `--strength` (0-100, default 100) blends the whole (`--balance`-dosed)
  correction back towards a no-op: 100 is the full correction, 0 disables
  it (a calibration file is still written, with every gain at 1.0), useful
  for dialing back an overly strong correction without having to
  recapture the flat-field photos.

## Limitations

* The calibration is tied to a specific device model + camera id + RAW
  resolution + CFA pattern (all encoded in the file name, see above). If the
  device exposes several RAW sizes per camera (e.g. full-res vs. a binned
  or a different-aspect-ratio mode), generate and ship one calibration
  file per size actually used; a size with no matching file is saved as an
  uncorrected DNG.
* This corrects optical vignetting and shading measured at one focus
  distance/aperture; it will be slightly less accurate at very different
  focus distances if the lens is not fully fixed-focus, which is a
  standard limitation of static lens shading maps.
* `tools/calibration/verify_gain_map.py` accepts multiple DNG files at
  once (e.g. a glob of an entire capture session) and prints a per-file
  pass/fail summary, exiting non-zero if any file did not improve.

# Zoom upscaling filters

`raisr-2x.bin` holds the learned 2x upscaling filters (RAISR: Romano,
Isidoro, Milanfar 2016) used by the enhanced renderer when a photo is taken
zoomed in: the centre of the frame is rendered on a finer grid from the burst
(super-resolution), and these filters sharpen the brightness of the upscaled
crop. 216 filters of 9x9 taps, chosen per pixel by edge angle, strength and
coherence; about 70 KB. Without the file the upscale is bilinear.

Retrain from a folder of sharp photos with the host tool in `tools/raisr`:

    qmake tools/raisr/raisr.pro && make
    ./raisr-train --augment --prescale 1.5 --test held-out.jpg photos/*.jpg raisr-2x.bin

A file with the same name in the `lens-shading/` device-profiles directory
overrides the bundled one. The bundled filters were trained on three ProCam
photos from the Jolla Phone 2 and are a starting point.
