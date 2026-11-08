// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// Multi-frame RAW pipeline: burst alignment, frequency-domain robust merge and
// finishing (demosaic, colour, local tone mapping, sharpening). The approach
// follows the published HDR+ description (Hasinoff et al. 2016, "Burst
// photography for high dynamic range and low-light imaging on mobile cameras")
// and Mertens et al. 2007 exposure fusion. No code or data from any vendor
// camera application is used. Pure C++ so it can be unit tested on the host.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace Raisr { struct Filters; }

namespace RawPipeline {

// Runs f(i) for i in [begin, end) on all cores. Work items are rows or tile
// rows, so a shared counter is cheap enough.
template <typename F>
void parallelFor(int begin, int end, F f)
{
    const int count = end - begin;
    if (count <= 0) return;
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const int threads = int(std::min<unsigned>(std::min(hardware, 16u), unsigned(count)));
    if (threads <= 1) {
        for (int i = begin; i < end; ++i) f(i);
        return;
    }
    std::atomic<int> next(begin);
    auto worker = [&]() {
        for (int i = next.fetch_add(1); i < end; i = next.fetch_add(1)) f(i);
    };
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (int t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (std::thread &thread : pool) thread.join();
}

// A mosaiced sensor frame as delivered by the HAL.
struct BayerFrame {
    int width = 0, height = 0;
    int cfa = 0; // 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR
    double white = 0;
    std::array<double, 4> black{{0, 0, 0, 0}}; // per CFA phase, raster order
    std::vector<uint16_t> pixels;
};

// The four CFA phases of a mosaic (raster order of the 2x2 quad) as separate
// half-resolution planes, black-subtracted and scaled so the white level is
// `clip`. Values below black stay negative so averaging remains unbiased.
struct Planes {
    int width = 0, height = 0;
    float clip = 1.0f;
    std::array<std::vector<float>, 4> phase;
};

// Per-phase noise variance: variance(x) = scale * x + offset, x in the same
// normalised units as Planes (Android SENSOR_NOISE_PROFILE / DNG NoiseProfile).
struct NoiseModel {
    std::array<double, 4> scale{{0, 0, 0, 0}};
    std::array<double, 4> offset{{0, 0, 0, 0}};
    double variance(int phase, double signal) const;
    double meanVariance(double signal) const;
};

struct TileOffset {
    int dx = 0, dy = 0;
    // Sub-pixel refinement in plane pixels, within (-1, 1); the merge uses
    // the whole-pixel part, super-resolution the sum.
    float fx = 0, fy = 0;
    // How well the tile constrains a sub-pixel shift (smaller structure
    // tensor eigenvalue); 0 when it was not refined.
    float confidence = 0;
};

// One offset (in plane pixels) per merge tile: 16x16 tiles, stride 8, the
// first tile starting at -8 so the raised-cosine windows cover every pixel.
struct Alignment {
    int tilesX = 0, tilesY = 0;
    std::vector<TileOffset> offsets;
    // Affine fit of the sub-pixel offsets (plane pixels) over the frame:
    // offset = a[0] + a[1] x + a[2] y, likewise b for y. Hand shake is smooth,
    // so the fit is far more precise than single tiles.
    bool hasModel = false;
    double a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
    const TileOffset &at(int tx, int ty) const { return offsets[std::size_t(ty) * tilesX + tx]; }
};

const int MergeTileSize = 16;
const int MergeTileStride = 8;
int mergeTileCount(int planeSize);

// Colour channel (0 R, 1 G, 2 B) of a CFA phase.
int phaseChannel(int cfa, int phase);

Planes toPlanes(const BayerFrame &frame, double exposureGain);
std::vector<float> grayImage(const Planes &planes);
std::vector<float> grayImage(const BayerFrame &frame, double exposureGain);
// Mean of the two green samples per quad (plane size): a single colour, so
// sub-pixel shifts translate it cleanly, unlike the mixed-colour gray image.
std::vector<float> greenImage(const BayerFrame &frame, double exposureGain);
// Gradient energy of a quarter-resolution copy; used to pick the sharpest
// reference frame.
double sharpness(const std::vector<float> &gray, int width, int height);
// Coarse-to-fine tile alignment of `alternate` onto `reference` (both gray
// images of plane size).
Alignment align(const std::vector<float> &reference, const std::vector<float> &alternate,
                int width, int height);

NoiseModel estimateNoise(const Planes &planes);
// Accepts 2 (shared) or 8 (per phase) values; returns false if unusable.
bool noiseFromProfile(const std::vector<double> &profile, NoiseModel *model);

struct MergeOptions {
    // c in Hasinoff eq. 6. Larger merges more (less robust to motion).
    double temporalStrength = 8.0;
    // Post-merge frequency-domain shrinkage; 0 disables.
    double spatialStrength = 1.0;
    // Share of the shrinkage applied at the lowest frequencies (the rest
    // grows with frequency); higher removes coarser grain.
    double spatialFloor = 0.0;
    // Two-step (empirical) Wiener: a first shrinkage gives a pilot estimate
    // whose power sets the final filter, which suppresses noise-only
    // frequencies far more than Hasinoff's ratio without musical noise.
    // 0 disables; otherwise the pilot's strength.
    double spatialPilot = 0.0;
};
struct MergeStats {
    // Average Wiener weight pulling alternates towards the reference: 0 when
    // every alternate merged fully, 1 when all were rejected as motion.
    double rejection = 0;
};
// frames[0] is the reference; alignments has one entry per other frame.
Planes merge(const std::vector<const Planes *> &frames, const std::vector<Alignment> &alignments,
             const NoiseModel &noise, const MergeOptions &options, MergeStats *stats);
// Same, reading tiles straight from the mosaics (half the memory of Planes);
// exposureGains normalises each frame to the reference exposure.
Planes merge(const std::vector<const BayerFrame *> &frames, const std::vector<double> &exposureGains,
             const std::vector<Alignment> &alignments, const NoiseModel &noise,
             const MergeOptions &options, MergeStats *stats);

// Multi-frame super-resolution of green at the full mosaic resolution
// (Wronski et al. 2019, simplified): every green sample of every frame is
// placed at its sub-pixel aligned position on the reference grid and spread
// over its 3x3 neighbours, weighted by distance and by how well that frame
// agrees with `guide` (the robust merge) locally, so moving objects do not
// ghost. Clipped samples are skipped. Planes units; NaN where nothing landed.
// With `region`, only that crop is produced, on a grid `scale` times finer
// (Super Res Zoom: more frames give real detail between the sensor pixels).
struct ZoomRegion {
    int x = 0, y = 0, width = 0, height = 0; // sensor pixels, even origin and size
    double scale = 1.0;
    bool active() const { return width > 0 && height > 0; }
    int outputWidth() const { return int(width * scale + 0.5); }
    int outputHeight() const { return int(height * scale + 0.5); }
};
// coverage (optional): per 2x2 output cell, how many alternate frames'
// worth of samples landed there (0 where only the reference contributed);
// FinishOptions::detailCoverage.
std::vector<float> superResolveGreen(const std::vector<const BayerFrame *> &frames,
                                     const std::vector<double> &exposureGains,
                                     const std::vector<Alignment> &alignments, const Planes &guide,
                                     const NoiseModel &noise, const ZoomRegion *region = nullptr,
                                     std::vector<float> *coverage = nullptr);

// Lens shading gains per CFA phase (raster order), grid points spanning the
// full mosaic from the first to the last pixel.
struct ShadingGrid {
    int rows = 0, cols = 0;
    std::array<std::vector<float>, 4> phase;
    bool valid() const;
    float gain(int phase, double x, double y) const; // x, y in [0, 1]
};

// Detected faces as (left, top, right, bottom), normalised on the full
// sensor frame. The tone mapping keeps the largest in a natural brightness
// range: not blown by the shadow lift, not left dark against a bright sky.
using FaceRects = std::vector<std::array<float, 4>>;

struct FinishOptions {
    int cfa = 0;
    std::array<double, 4> wbGains{{1, 1, 1, 1}}; // Android order: R, G_even, G_odd, B
    std::array<double, 9> colorMatrix{{1, 0, 0, 0, 1, 0, 0, 0, 1}}; // camera RGB to linear sRGB
    const ShadingGrid *shading = nullptr;
    // Gain that restores the metered exposure (2^underexposure for bursts).
    double targetGain = 1.0;
    // Noise / signal at metered mid grey after merging (drives denoise and
    // sharpening strength).
    double relativeNoise = 0.0;
    // Noise standard deviation at black after merging (normalised units);
    // colour is faded out in shadows within a few times this level.
    double blackNoise = 0.0;
    double localToneMapping = 1.0; // 0 disables exposure fusion
    // Night mode: dark scenes are lifted up to 8x the metered exposure (3x
    // otherwise), towards a dusk-like key rather than daylight.
    bool night = false;
    FaceRects faces;
    // Super-resolved green (superResolveGreen); its fine detail replaces the
    // demosaicked detail, scaled by detailStrength. Modified in place.
    std::vector<float> *detailGreen = nullptr;
    double detailStrength = 1.0;
    // Its coverage (superResolveGreen): the swap fades out where fewer than
    // two alternate frames contributed, since the reference alone has no
    // detail beyond the demosaic, only a different (checkered) sampling.
    const std::vector<float> *detailCoverage = nullptr;
    // Zoomed output: only this sensor crop, upscaled by its scale with the
    // learned RAISR filters (bilinear without them). detailGreen then has
    // the output size (superResolveGreen with the same region).
    ZoomRegion zoom;
    const Raisr::Filters *upscaleFilters = nullptr;
    double sharpening = 1.0;
    double chromaDenoise = 1.0;
    double saturation = 1.3;
    double contrast = 0.3;
    // Tone refinements (off by default while being evaluated):
    // blackPoint maps the darkest 0.2% down to about 5/255 when the image has
    // no real blacks (lifted shadows look hazy otherwise); whitePoint lets
    // the brightest 0.5% reach about 230/255 when they stop short (indoors),
    // stretching only the upper tones; clarity adds local contrast at a
    // medium scale (5 px) in the mid-tones, noise-aware, for texture without
    // sharpening pixel noise.
    bool blackPoint = false;
    bool whitePoint = false;
    double clarity = 0.0;
    // Share of the short side the photo keeps (16:9 crop of a 4:3 frame:
    // 0.75), centred; the black and white points are measured there.
    double keptShortSide = 1.0;
};
struct FinishStats {
    double shortGain = 1, longGain = 1;
    double highlightLevel = 0, keyLevel = 0;
    double faceLevel = -1; // linear, at the captured exposure; -1 without a face
    double faceBright = -1; // its 90th percentile, the lit side
    double faceOutput = -1; // the face's lit side after tone mapping (gamma)
    // Tone refinements applied: display black point (0 = none), white
    // stretch (1 = none), and the measured output extremes.
    double blackPoint = 0, whiteScale = 1, lowLevel = 0, highLevel = 0;
    // Milliseconds per stage: mosaic, demosaic, chroma, tone, apply, sharpen.
    std::array<double, 6> stageMs{{0, 0, 0, 0, 0, 0}};
};
struct RgbImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;
};
RgbImage finish(const Planes &merged, const FinishOptions &options, FinishStats *stats);

// Cameras without RAW output (front, ultra-wide) deliver 8-bit YUV 4:2:0
// frames, captured with the ISP's noise reduction and sharpening off. They
// are display-referred (the ISP's tone curve applied), so the burst is merged
// as it is: luma as four 2x2 polyphase planes, chroma at half resolution, with
// the same tile alignment and Wiener merge as RAW bursts.
struct YuvFrame {
    int width = 0, height = 0; // even
    std::vector<uint8_t> data; // I420: Y plane, then U, then V
};
struct YuvOptions {
    double spatialDenoise = 1.0;
    // Brightness gain when the burst could not reach the metered exposure.
    double brightness = 1.0;
    double sharpening = 1.0;
    ZoomRegion zoom;
    const Raisr::Filters *upscaleFilters = nullptr;
    // Night mode: dark scenes are lifted up to 3x even when the frames
    // reached the metered exposure.
    bool night = false;
    FaceRects faces;
    // Tone refinements as in FinishOptions; saturation is a multiplier.
    bool blackPoint = false;
    bool whitePoint = false;
    double clarity = 0.0;
    double saturation = 1.0;
    double keptShortSide = 1.0;
    // The camera's own brightening (measureCameraCurve()), when the burst
    // had a frame at the metered exposure: replaces the sRGB model of the
    // frames for the brightness gain. Empty: sRGB, as for every camera that
    // applies burst exposures at once.
    std::vector<float> cameraCurve;
};
struct YuvStats {
    double rejection = 0, lumaNoise = 0, chromaNoise = 0, severity = 0;
    double faceLevel = -1;
    double blackPoint = 0, whiteScale = 1;
    // Share of the measured camera brightening applied (cameraCurve), -1 if none.
    double cameraLift = -1;
    // Milliseconds: align, merge, finish.
    std::array<double, 3> stageMs{{0, 0, 0}};
};
// How a camera brightens its own (tone-mapped) output: from the luma of a
// frame at the burst's exposure and one the camera took at a higher
// (metered) exposure of the same scene, a curve of 257 values over [0, 1]
// mapping the first to the second. Medians of 16x16 cell means, so small
// shake and noise do not matter. Empty if the frames do not cover enough of
// the range or the result is implausible.
std::vector<float> measureCameraCurve(const YuvFrame &captured, const YuvFrame &metered);
// frames[0] is the reference.
RgbImage renderYuvBurst(const std::vector<const YuvFrame *> &frames, const YuvOptions &options,
                        YuvStats *stats);

// Burst exposure plan from the metered (preview AE) exposure. Frames are
// captured underexposed so highlights survive; the tone mapper restores the
// metered brightness with targetGain = metered / captured exposure.
struct BurstPlan {
    int frames = 0;
    int64_t exposureNs = 0;
    int iso = 0;
    double targetGain = 1.0;
    double underexposureEv = 0.0;
};
// clippedFraction: share of preview pixels in the top histogram bin.
// equalExposure: frames at the metered exposure (YUV bursts, whose tone
// curve is already applied, gain nothing from underexposure).
// night: night mode, for scenes too dark for the preview's auto exposure:
// all maxFrames frames (16 from the app), long (up to 1/5 s, held still),
// collecting up to 2 EV more light than metered; the renderer brightens
// further (FinishOptions::night).
// What the viewfinder saw just before the shot; negative = unknown.
struct BurstScene {
    // Image motion (hand shake) in frame widths per second, and the share of
    // the frame moving on its own (subjects).
    double motion = -1.0;
    double subjectMotion = 0.0;
    // Brightest detected face: mean luma (0..1) and clipped share in the
    // viewfinder.
    double faceLuma = -1.0;
    double faceClipped = 0.0;
};
// With motion known, each frame is as long as keeps the blur within about
// 1.5 pixels (2.5 in night mode, which has strong denoising): longer when
// the phone is held still, shorter when it moves, instead of the fixed
// ISO-based steps. A bright or clipped face gets highlight headroom.
BurstPlan planBurst(int64_t meteredExposureNs, int meteredIso, double clippedFraction,
                    int64_t minExposureNs, int64_t maxExposureNs, int minIso, int maxIso,
                    int maxFrames, bool equalExposure = false, bool night = false,
                    const BurstScene &scene = BurstScene());

// Exposed for tests.
void fft16x16(float *re, float *im, bool inverse);
std::vector<float> demosaic(const std::vector<float> &mosaic, int width, int height, int cfa);
std::vector<float> fuseExposures(const std::vector<float> &luma, int width, int height,
                                 double shortGain, double longGain);
// The same for an image a camera has already tone mapped (gamma-encoded
// luma): the long exposure is longCurve (luma at the captured exposure ->
// the camera's luma at the metered one, sampled evenly over [0, 1]).
std::vector<float> fuseCameraExposures(const std::vector<float> &luma, int width, int height,
                                       const std::vector<float> &longCurve);
double srgbEncode(double linear);
double srgbDecode(double encoded);

}
