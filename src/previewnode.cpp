// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "previewnode.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>

namespace {

// Unsharp amount: the preview is upscaled about 2x to the screen, which the
// HAL's own sharpening does not anticipate.
const float SharpenAmount = 0.5f;

const char *const VertexShader =
        "attribute highp vec4 qt_Vertex;\n"
        "attribute highp vec2 qt_MultiTexCoord0;\n"
        "uniform highp mat4 qt_Matrix;\n"
        "varying highp vec2 texCoord;\n"
        "void main() {\n"
        "    texCoord = qt_MultiTexCoord0;\n"
        "    gl_Position = qt_Matrix * qt_Vertex;\n"
        "}\n";

} // namespace

// One reused GL texture, refilled in place from RGB888 (QImage) or from a
// single 8-bit plane inside a shared byte array.
class PreviewTexture : public QSGTexture
{
public:
    explicit PreviewTexture(GLenum format) : m_format(format)
    {
        setFiltering(QSGTexture::Linear);
        setHorizontalWrapMode(QSGTexture::ClampToEdge);
        setVerticalWrapMode(QSGTexture::ClampToEdge);
    }
    ~PreviewTexture() override
    {
        if (m_id && QOpenGLContext::currentContext())
            QOpenGLContext::currentContext()->functions()->glDeleteTextures(1, &m_id);
    }

    void setImage(const QImage &image)
    {
        m_image = image;
        m_bytes.clear();
        m_pendingSize = image.size();
        m_dirty = true;
    }
    void setPlane(const QByteArray &bytes, int offset, const QSize &size)
    {
        m_bytes = bytes;
        m_offset = offset;
        m_image = QImage();
        m_pendingSize = size;
        m_dirty = true;
    }

    int textureId() const override { return int(m_id); }
    QSize textureSize() const override { return m_dirty ? m_pendingSize : m_size; }
    bool hasAlphaChannel() const override { return false; }
    bool hasMipmaps() const override { return false; }

    void bind() override
    {
        QOpenGLFunctions *gl = QOpenGLContext::currentContext()->functions();
        bool created = false;
        if (!m_id) {
            gl->glGenTextures(1, &m_id);
            created = true;
        }
        gl->glBindTexture(GL_TEXTURE_2D, m_id);
        if (m_dirty && !m_pendingSize.isEmpty()) {
            // QImage pads scanlines to 4 bytes; planes are tightly packed.
            const void *data = m_image.isNull()
                    ? static_cast<const void *>(m_bytes.constData() + m_offset)
                    : static_cast<const void *>(m_image.constBits());
            gl->glPixelStorei(GL_UNPACK_ALIGNMENT, m_image.isNull() ? 1 : 4);
            if (m_pendingSize != m_size || created) {
                m_size = m_pendingSize;
                gl->glTexImage2D(GL_TEXTURE_2D, 0, m_format, m_size.width(), m_size.height(), 0,
                                 m_format, GL_UNSIGNED_BYTE, data);
            } else {
                gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_size.width(), m_size.height(),
                                    m_format, GL_UNSIGNED_BYTE, data);
            }
            gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            // The upload copied it; let the GUI thread reuse the memory.
            m_image = QImage();
            m_bytes.clear();
            m_dirty = false;
        }
        updateBindOptions(created);
    }

private:
    GLenum m_format;
    GLuint m_id = 0;
    QSize m_size, m_pendingSize;
    QImage m_image;
    QByteArray m_bytes;
    int m_offset = 0;
    bool m_dirty = false;
};

class PreviewMaterial : public QSGMaterial
{
public:
    PreviewTexture *texture = nullptr;

    QSGMaterialType *type() const override
    {
        static QSGMaterialType type;
        return &type;
    }
    QSGMaterialShader *createShader() const override;
    int compare(const QSGMaterial *other) const override
    {
        const PreviewMaterial *material = static_cast<const PreviewMaterial *>(other);
        return texture == material->texture ? 0 : (texture < material->texture ? -1 : 1);
    }
};

class PreviewYuvMaterial : public QSGMaterial
{
public:
    PreviewTexture *planes[3] = { nullptr, nullptr, nullptr };

    QSGMaterialType *type() const override
    {
        static QSGMaterialType type;
        return &type;
    }
    QSGMaterialShader *createShader() const override;
    int compare(const QSGMaterial *other) const override
    {
        const PreviewYuvMaterial *material = static_cast<const PreviewYuvMaterial *>(other);
        return planes[0] == material->planes[0] ? 0 : (planes[0] < material->planes[0] ? -1 : 1);
    }
};

// Shared uniforms of both shaders.
class PreviewShaderBase : public QSGMaterialShader
{
public:
    const char *vertexShader() const override { return VertexShader; }
    const char *const *attributeNames() const override
    {
        static const char *const names[] = { "qt_Vertex", "qt_MultiTexCoord0", nullptr };
        return names;
    }
    void initialize() override
    {
        m_matrix = program()->uniformLocation("qt_Matrix");
        m_opacity = program()->uniformLocation("qt_Opacity");
        m_texel = program()->uniformLocation("texel");
        m_amount = program()->uniformLocation("amount");
    }

protected:
    void updateCommon(const RenderState &state, const QSize &size)
    {
        if (state.isMatrixDirty())
            program()->setUniformValue(m_matrix, state.combinedMatrix());
        if (state.isOpacityDirty())
            program()->setUniformValue(m_opacity, state.opacity());
        if (!size.isEmpty())
            program()->setUniformValue(m_texel, 1.0f / size.width(), 1.0f / size.height());
        program()->setUniformValue(m_amount, SharpenAmount);
    }

private:
    int m_matrix = -1, m_opacity = -1, m_texel = -1, m_amount = -1;
};

class PreviewShader : public PreviewShaderBase
{
public:
    const char *fragmentShader() const override
    {
        // Unsharp mask against the four direct neighbours (one texel away).
        return "uniform sampler2D source;\n"
               "uniform highp vec2 texel;\n"
               "uniform mediump float amount;\n"
               "uniform lowp float qt_Opacity;\n"
               "varying highp vec2 texCoord;\n"
               "void main() {\n"
               "    mediump vec3 centre = texture2D(source, texCoord).rgb;\n"
               "    mediump vec3 around = texture2D(source, texCoord + vec2(texel.x, 0.0)).rgb\n"
               "            + texture2D(source, texCoord - vec2(texel.x, 0.0)).rgb\n"
               "            + texture2D(source, texCoord + vec2(0.0, texel.y)).rgb\n"
               "            + texture2D(source, texCoord - vec2(0.0, texel.y)).rgb;\n"
               "    mediump vec3 sharp = centre + amount * (centre - 0.25 * around);\n"
               "    gl_FragColor = vec4(clamp(sharp, 0.0, 1.0), 1.0) * qt_Opacity;\n"
               "}\n";
    }
    void initialize() override
    {
        PreviewShaderBase::initialize();
        program()->setUniformValue("source", 0);
    }
    void updateState(const RenderState &state, QSGMaterial *newMaterial, QSGMaterial *) override
    {
        PreviewTexture *texture = static_cast<PreviewMaterial *>(newMaterial)->texture;
        texture->bind();
        updateCommon(state, texture->textureSize());
    }
};

class PreviewYuvShader : public PreviewShaderBase
{
public:
    const char *fragmentShader() const override
    {
        // Full-range BT.601 (Camera2's YUV_420_888), the unsharp mask on luma
        // only so colour noise is not sharpened.
        return "uniform sampler2D planeY;\n"
               "uniform sampler2D planeU;\n"
               "uniform sampler2D planeV;\n"
               "uniform highp vec2 texel;\n"
               "uniform mediump float amount;\n"
               "uniform lowp float qt_Opacity;\n"
               "varying highp vec2 texCoord;\n"
               "void main() {\n"
               "    mediump float y = texture2D(planeY, texCoord).r;\n"
               "    mediump float around = texture2D(planeY, texCoord + vec2(texel.x, 0.0)).r\n"
               "            + texture2D(planeY, texCoord - vec2(texel.x, 0.0)).r\n"
               "            + texture2D(planeY, texCoord + vec2(0.0, texel.y)).r\n"
               "            + texture2D(planeY, texCoord - vec2(0.0, texel.y)).r;\n"
               "    y = y + amount * (y - 0.25 * around);\n"
               "    mediump float u = texture2D(planeU, texCoord).r - 0.5;\n"
               "    mediump float v = texture2D(planeV, texCoord).r - 0.5;\n"
               "    mediump vec3 rgb = vec3(y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);\n"
               "    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0) * qt_Opacity;\n"
               "}\n";
    }
    void initialize() override
    {
        PreviewShaderBase::initialize();
        program()->setUniformValue("planeY", 0);
        program()->setUniformValue("planeU", 1);
        program()->setUniformValue("planeV", 2);
    }
    void updateState(const RenderState &state, QSGMaterial *newMaterial, QSGMaterial *) override
    {
        PreviewYuvMaterial *material = static_cast<PreviewYuvMaterial *>(newMaterial);
        QOpenGLFunctions *gl = QOpenGLContext::currentContext()->functions();
        // Units 1 and 2 first, so unit 0 is active again afterwards, as the
        // scene graph expects.
        gl->glActiveTexture(GL_TEXTURE1);
        material->planes[1]->bind();
        gl->glActiveTexture(GL_TEXTURE2);
        material->planes[2]->bind();
        gl->glActiveTexture(GL_TEXTURE0);
        material->planes[0]->bind();
        updateCommon(state, material->planes[0]->textureSize());
    }
};

QSGMaterialShader *PreviewMaterial::createShader() const
{
    return new PreviewShader;
}

QSGMaterialShader *PreviewYuvMaterial::createShader() const
{
    return new PreviewYuvShader;
}

PreviewNode::PreviewNode()
    : m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4)
    , m_rgbTexture(new PreviewTexture(GL_RGB))
    , m_planes{ new PreviewTexture(GL_LUMINANCE), new PreviewTexture(GL_LUMINANCE),
                new PreviewTexture(GL_LUMINANCE) }
    , m_rgbMaterial(new PreviewMaterial)
    , m_yuvMaterial(new PreviewYuvMaterial)
{
    m_rgbMaterial->texture = m_rgbTexture;
    for (int i = 0; i < 3; ++i) m_yuvMaterial->planes[i] = m_planes[i];
    // The viewfinder fades (camera switch, gallery): the output is
    // premultiplied by qt_Opacity, so blending keeps that working.
    m_rgbMaterial->setFlag(QSGMaterial::Blending, true);
    m_yuvMaterial->setFlag(QSGMaterial::Blending, true);
    m_geometry.setDrawingMode(GL_TRIANGLE_STRIP);
    setGeometry(&m_geometry);
    setMaterial(m_yuvMaterial);
}

PreviewNode::~PreviewNode()
{
    // Not owned through the node flags: both materials are kept and swapped.
    delete m_rgbMaterial;
    delete m_yuvMaterial;
    delete m_rgbTexture;
    for (PreviewTexture *plane : m_planes) delete plane;
}

void PreviewNode::setFrame(const QImage &frame)
{
    m_rgbTexture->setImage(frame);
    if (material() != m_rgbMaterial) setMaterial(m_rgbMaterial);
    markDirty(DirtyMaterial);
}

void PreviewNode::setYuvFrame(const QByteArray &planes, const QSize &size)
{
    const QSize chroma((size.width() + 1) / 2, (size.height() + 1) / 2);
    const int lumaBytes = size.width() * size.height();
    m_planes[0]->setPlane(planes, 0, size);
    m_planes[1]->setPlane(planes, lumaBytes, chroma);
    m_planes[2]->setPlane(planes, lumaBytes + chroma.width() * chroma.height(), chroma);
    if (material() != m_yuvMaterial) setMaterial(m_yuvMaterial);
    markDirty(DirtyMaterial);
}

void PreviewNode::setPlacement(const QRectF &drawRect, const QRectF &sourceRect, int orientation,
                               bool mirror)
{
    // Display-frame corners to sensor-frame texture coordinates: undo the
    // mirror, then the rotation (as Camera2Preview::mapToSensor does).
    const int rotation = ((orientation % 360) + 360) % 360;
    auto toTexture = [rotation, mirror](qreal u, qreal v) {
        if (mirror) u = 1.0 - u;
        switch (rotation) {
        case 90: return QPointF(v, 1.0 - u);
        case 180: return QPointF(1.0 - u, 1.0 - v);
        case 270: return QPointF(1.0 - v, u);
        default: return QPointF(u, v);
        }
    };
    QSGGeometry::TexturedPoint2D *v = m_geometry.vertexDataAsTexturedPoint2D();
    const QPointF corners[4] = { drawRect.topLeft(), drawRect.topRight(), drawRect.bottomLeft(),
                                 drawRect.bottomRight() };
    const QPointF sources[4] = { sourceRect.topLeft(), sourceRect.topRight(), sourceRect.bottomLeft(),
                                 sourceRect.bottomRight() };
    for (int i = 0; i < 4; ++i) {
        const QPointF t = toTexture(sources[i].x(), sources[i].y());
        v[i].set(float(corners[i].x()), float(corners[i].y()), float(t.x()), float(t.y()));
    }
    markDirty(DirtyGeometry);
}
