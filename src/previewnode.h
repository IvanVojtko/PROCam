// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause

#ifndef PREVIEWNODE_H
#define PREVIEWNODE_H

#include <QByteArray>
#include <QImage>
#include <QRectF>
#include <QSGGeometryNode>

class PreviewTexture;
class PreviewMaterial;
class PreviewYuvMaterial;

// Scene-graph node for the Camera2 viewfinder. The frame stays in sensor
// orientation: rotation and mirroring are texture coordinates. Frames go
// into reused GL textures as they arrive (I420 planes, or RGB888 bytes) with
// no CPU conversion; the shader converts YUV to RGB and applies a light
// unsharp mask that offsets the upscaling to screen size. Lives on the
// render thread.
class PreviewNode : public QSGGeometryNode
{
public:
    PreviewNode();
    ~PreviewNode() override;

    // A new RGB888 frame (uploaded on the next draw).
    void setFrame(const QImage &frame);
    // A new I420 frame: Y (size), then U and V at half resolution, packed.
    void setYuvFrame(const QByteArray &planes, const QSize &size);
    // drawRect: item coordinates. sourceRect: the shown part of the frame
    // in display orientation (frame rotated clockwise by `orientation`, then
    // mirrored horizontally), normalised to 0..1.
    void setPlacement(const QRectF &drawRect, const QRectF &sourceRect, int orientation, bool mirror);

private:
    QSGGeometry m_geometry;
    PreviewTexture *m_rgbTexture;
    PreviewTexture *m_planes[3];
    PreviewMaterial *m_rgbMaterial;
    PreviewYuvMaterial *m_yuvMaterial;
};

#endif // PREVIEWNODE_H
