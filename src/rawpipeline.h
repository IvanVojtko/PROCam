// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <QJsonObject>
#include <QStringList>

class QImage;

// Enhanced RAW renderer: single frames get noise-model driven denoising and
// local tone mapping, bursts are aligned and merged first (rawpipelinemath.h).
namespace RawPipeline {

struct RenderSettings {
    // Lens shading calibration lookup (see DngLensShading), tried in order.
    QStringList calibrationDirs;
    QString deviceModel;
    // Gain that restores the metered exposure; bursts are captured
    // underexposed by this factor (1 for ordinary captures).
    double targetGain = 1.0;
    // User exposure multiplier (rawCaptureExposure).
    double exposure = 1.0;
    // Multiplies the post-merge spatial denoise strength.
    double spatialDenoise = 1.0;
    double spatialFloor = -1.0; // < 0: automatic
    double spatialPilot = -1.0; // < 0: automatic
    // Multi-frame super-resolution of fine detail for bursts of 3+ frames.
    bool superResolution = true;
    // Viewfinder zoom: the centre 1/zoom of the frame is rendered, up to 2x
    // finer than the sensor (Super Res Zoom), so the output stays near the
    // full frame size. Learned upscaling filters (raisr-2x.bin) are looked up
    // in calibrationDirs. <= 0: the reference frame's zoom_ratio.
    double zoom = 0.0;
    // Tone refinements: black point, white point, clarity and 5% more
    // saturation (FinishOptions / YuvOptions).
    bool toneRefinements = true;
};

// Every RAW frame of a render was only partly written by the HAL
// (rawframecheck.h): the camera's RAW output cannot be used. The error text
// of renderImage() and renderBurst() then starts with this.
QString rawFramesDamagedError(const QString &detail);
bool isRawFramesDamagedError(const QString &error);

// Renders RAW frames described by JSON sidecars (with raw_path) to an RGB
// image. More than one frame is merged as a burst of equal exposures.
// *info receives pipeline details for the output sidecar.
bool renderImage(const QStringList &metadataPaths, const RenderSettings &settings, QImage *image,
                 QJsonObject *info, QString *error);

// Burst render to a JPEG + JSON sidecar, like RawBracket::render(): rotates by
// the frames' bracket_rotation and applies the requested colour adjustments.
bool renderBurst(const QString &target, const QStringList &metadataPaths, int quality,
                 const RenderSettings &settings, QString *error);

}
