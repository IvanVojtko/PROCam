<p align="center">
  <img src="icons/procam.svg" width="128" alt="ProCam icon">
</p>

<h1 align="center">ProCam</h1>

<p align="center">
  Computational photography for the Jolla Phone 2 on Sailfish OS.<br>
  Every photo is rendered on the phone from merged RAW bursts.
</p>

<p align="center">
  <a href="https://github.com/IvanVojtko/PROCam/actions/workflows/ci.yml"><img src="https://github.com/IvanVojtko/PROCam/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/IvanVojtko/PROCam/releases/latest"><img src="https://img.shields.io/github/v/release/IvanVojtko/PROCam?include_prereleases&label=release" alt="Latest release"></a>
  <a href="LICENSES/BSD-3-Clause.txt"><img src="https://img.shields.io/badge/license-BSD--3--Clause-blue" alt="License"></a>
</p>

---

ProCam brings phone-style computational photography to Sailfish OS. Instead
of saving the camera hardware's own processed JPEG, it captures RAW sensor
frames and renders them with its own pipeline, modelled on the published
HDR+ research: bursts are aligned and merged, noise is removed, highlights
are kept, and fine detail is rebuilt from the small shifts of your hand
between frames.

## Features

- **RAW rendering for every photo.** JPEG or RAW, HDR on or off: each shot is
  built from RAW16 sensor data with a noise model, highlight reconstruction,
  local tone mapping, lens shading correction and noise-aware sharpening.
- **HDR bursts.** Up to 8 frames, aligned and merged, so skies keep their
  detail and indoor shots stay clean. Frames are kept short (1/60 s where the
  light allows) to avoid hand-shake blur.
- **Night mode.** A moon appears in the top bar when the scene is too dark.
  Tap it and hold still for a second or two: 8 long frames (up to 1/5 s)
  are merged and brightened to a natural, evening-like look.
- **Super-resolution and sharp 2x zoom.** Burst frames fill in real detail
  between the sensor pixels. The 2x zoom is rendered from the burst on a finer
  grid, sharpened with learned RAISR filters.
- **Front and ultra-wide cameras too.** They have no RAW output, so ProCam
  merges bursts of full-resolution YUV frames instead, with the camera's own
  smoothing switched off.
- **4:3 or 16:9 photos**, and optional RAW (RAW16 + JSON) and DNG files next
  to each JPEG.
- **A modern viewfinder.** Fullscreen preview, Photo/Video tabs (or swipe),
  tap to focus with a brightness slider, an instant thumbnail, and a capture
  queue so you can keep shooting while earlier photos are processed.
- **Normal and Pro modes.** Pro adds the live histogram, manual ISO, shutter
  and focus, white balance, RAW bracketing and more.

## Where ProCam comes from

ProCam is a fork of **[RAWfish](https://github.com/Logic-gate/RAWfish)** by
Logic-gate and contributors. RAWfish is itself a fork of Jolla Camera that
did the hard groundwork: an Android Camera2 bridge running through libhybris,
RAW16 and DNG capture, metadata handling and manual controls. ProCam would not
exist without it.

### Why a new name

ProCam started as a set of improvements meant for RAWfish. Over time the
changes grew well beyond what fits in a pull request:

- a new RAW rendering pipeline that every photo goes through,
- burst capture with HDR, night mode, super-resolution and zoom,
- a redesigned camera interface,
- and a shift in focus from a RAW capture tool to a point-and-shoot camera
  that does the processing for you.

Rather than push a different direction onto the original project, it is
released as its own app with its own name, app ID (`com.procam`) and settings
(`/apps/procam`). RAWfish remains the place for its own goals; ProCam is the
computational photography branch of that family.

## Device support

| | |
|---|---|
| Phone | Jolla Phone 2 (2026) |
| System | Sailfish OS 5.1, aarch64 |
| Main camera | RAW16 bursts: HDR, night mode, super-resolution, 2x zoom |
| Front and ultra-wide | YUV bursts: HDR and night mode |

ProCam talks to the camera through Android's Camera2 API, so it depends on
what the phone's camera HAL exposes. Other Sailfish devices may need porting:
camera IDs, RAW support, preview orientation and metadata quality all vary by
device.

## Installation

1. Download the latest `procam-*.aarch64.rpm` from
   [Releases](https://github.com/IvanVojtko/PROCam/releases).
2. Uninstall RAWfish first if you have it: both apps ship the same camera
   bridge and QML module, so they cannot be installed side by side.
3. Install the package, either by opening it in the file manager or from a
   terminal:

   ```sh
   devel-su pkcon install-local procam-*.aarch64.rpm
   ```

Settings live in the app's menu and under **Settings → Apps → ProCam**.

## Using ProCam

| Control | What it does |
|---|---|
| **Photo / Video** | Tabs above the shutter; swiping the viewfinder switches too |
| **Normal / Pro** | Simple shooting, or full manual controls with a histogram |
| **HDR** | Merged burst for every photo (on by default) |
| **Moon** | Shown in dark scenes; turns night mode on for the next shots |
| **1x / 2x** | Zoom presets; 2x is a super-resolved crop, not a blurry upscale |
| **⋯ menu** | Timer, aspect ratio (4:3 / 16:9), RAW files, grid, advanced options |
| **Tap** | Focus and brightness at that point; drag the sun to adjust |

## How the photo pipeline works

1. **Plan.** From the preview's exposure and how much of it is clipped,
   ProCam picks the number of frames, their exposure time and their gain.
   Frames are slightly underexposed so that highlights survive.
2. **Capture.** The frames are captured as RAW16 (or full-size YUV) with focus
   held still, while the preview keeps running.
3. **Align.** The sharpest early frame becomes the reference. The others are
   aligned to it with a coarse-to-fine tile search, sub-pixel refinement and
   a robust fit of the hand shake.
4. **Merge.** A frequency-domain Wiener merge on 16x16 tiles averages away
   noise and rejects moving objects, so they don't leave ghosts.
5. **Super-resolve.** Every green sample is placed at its true sub-pixel
   position, which recovers detail beyond the demosaic. The same step renders
   the 2x zoom.
6. **Finish.** Highlight reconstruction, lens shading, demosaic, colour,
   exposure fusion and sharpening produce the final JPEG. Night mode lifts the
   shadows further.

Rendering runs in the background at low priority, up to three photos queued.
Methods follow Hasinoff et al. 2016 (HDR+), Wronski et al. 2019
(super-resolution), Mertens et al. 2007 (exposure fusion) and Romano et al.
2016 (RAISR). No code or models from other camera apps are used.

The core is plain C++ in
[`src/rawpipelinemath.cpp`](src/rawpipelinemath.cpp), with file handling in
[`src/rawpipeline.cpp`](src/rawpipeline.cpp), and it runs on a desktop too:
see [Tests](#tests).

## Building

You need the [Sailfish SDK](https://docs.sailfishos.org/Tools/Sailfish_SDK/)
with the `SailfishOS-5.1.0.11-aarch64` target, and Android NDK r27 for the
Camera2 bridge.

```sh
# 1. The Android Camera2 bridge (libsfoscamera2.so)
ANDROID_NDK_HOME=~/Android/Sdk/ndk/27.2.12479018 sh sfos-camera2-bridge/scripts/build-android.sh

# 2. The RPM, as a shadow build so the source tree stays clean
mkdir -p build/sfos && cd build/sfos
ln -sfn ../../LICENSES LICENSES && ln -sfn ../../licenses licenses
sfdk -c target=SailfishOS-5.1.0.11-aarch64 build ../..
```

The package lands in `build/sfos/RPMS/`. The version comes from git: a
commit tagged `v1.4.0` builds `procam-1.4.0`, and any other commit gets the
latest tag plus branch, date and hash.

### Tests

The host tests need only Qt 5 (`qtbase5-dev`, `qtdeclarative5-dev-tools`) and
Node.js:

```sh
for t in tests/*.js; do node "$t"; done          # settings and capture flow
mkdir -p build/host-rawpipeline && cd build/host-rawpipeline
qmake ../../tests/rawpipeline.pro && make -j"$(nproc)"
QT_QPA_PLATFORM=offscreen ./tst_rawpipeline       # RAW pipeline on synthetic bursts
```

`tests/rawbracket.pro`, `tests/previewaids.pro` and `tests/previewsize.pro`
build the same way. The pipeline tests take a few environment variables:

| Variable | Effect |
|---|---|
| `RAWPIPELINE_DUMP_DIR=<dir>` | Saves the test images |
| `RAWPIPELINE_RENDER=<sidecar.json>` | Renders a capture from the phone on the desktop |
| `RAWPIPELINE_BENCH=<frames>` | Times a full-size burst |

## Continuous integration and releases

| Workflow | When | What |
|---|---|---|
| [CI](.github/workflows/ci.yml) | Every push and pull request | Host tests, then a test RPM attached to the run |
| [Release](.github/workflows/release.yml) | Manually, or on a `v*` tag | Tests, tag, RPM build and a GitHub release with checksums |

To publish a release, open **Actions → Release → Run workflow** and enter a
version such as `1.4.0`. The workflow tests the selected branch, creates the
tag `v1.4.0` only if the tests pass, builds the RPM and publishes the release
with generated notes. Pushing a tag yourself works too:

```sh
git tag -a v1.4.0 -m "ProCam 1.4.0" && git push origin v1.4.0
```

Versions with a suffix, such as `1.4.0-beta.1`, are published as
pre-releases.

## Project layout

| Path | Contents |
|---|---|
| `src/` | QML plugin: camera UI, settings, capture logic and the RAW pipeline |
| `src/capture/`, `src/settings/` | Viewfinder, controls and menus (QML) |
| `src/calibration/` | Lens shading profiles and the RAISR zoom filters |
| `sfos-camera2-bridge/` | Android Camera2 bridge (libhybris) and the Sailfish helper tools |
| `settings/` | Page under Settings → Apps → ProCam |
| `tests/` | Host tests (C++ and Node.js) |
| `tools/` | Lens shading calibration, RAISR training, icon generator |
| `rpm/procam.spec` | Package definition |

## Credits

- [RAWfish](https://github.com/Logic-gate/RAWfish) and its contributors, for
  the Camera2 bridge, RAW/DNG capture and everything ProCam builds on.
- [ric9k](https://forum.sailfishos.org/u/ric9k/summary), for smoothing out
  RAWfish's rough edges, valuable feedback and the lens shading calibration.
- Tadi, for RAW bracketing
  ([licenses/RawBracket-MIT.txt](licenses/RawBracket-MIT.txt)).
- Jolla, for Jolla Camera.

## AI usage

Like RAWfish, ProCam is developed with substantial AI assistance (research,
debugging, code generation and review). Changes are checked with the host
tests and on the phone before release. Contributions, AI-assisted or not, are
welcome under the [AI Usage Policy](AI_USAGE_POLICY.md).

## License

BSD-3-Clause, inherited from Jolla Camera and RAWfish; the original copyright
notices are kept in every file. RAW bracketing is MIT-licensed
([licenses/RawBracket-MIT.txt](licenses/RawBracket-MIT.txt)). Full texts are
in [LICENSES/](LICENSES) and [licenses/](licenses).
