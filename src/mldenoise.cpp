// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause
#include "mldenoise.h"
#include "rawpipelinemath.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace MlDenoise {
namespace {

template <typename T>
bool readValue(QFile &file, T *value)
{
    return file.read(reinterpret_cast<char *>(value), sizeof(T)) == qint64(sizeof(T));
}

// Expected (out, in, kernel) per convolution for a model of width w.
std::vector<std::array<int, 3>> layerShapes(int w)
{
    const int c0 = w, c1 = 2 * w, c2 = 3 * w;
    return {{c0, 8, 3},      {c0, c0, 3}, {c1, c0, 2}, {c1, c1, 3}, {c2, c1, 2}, {c2, c2, 3},
            {c2, c2, 3},     {c1, c2, 1}, {c1, 2 * c1, 3}, {c0, c1, 1}, {c0, 2 * c0, 3}, {4, c0, 3}};
}

} // namespace

bool loadModel(const QString &path, Model *model, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read %1").arg(path);
        return false;
    }
    char magic[4];
    quint32 version = 0, width = 0;
    if (file.read(magic, 4) != 4 || std::memcmp(magic, "PCDN", 4) != 0 || !readValue(file, &version)
            || version != 1 || !readValue(file, &width) || width < 4 || width > 64 || width % 4) {
        *error = QStringLiteral("%1 is not a denoiser model").arg(path);
        return false;
    }
    Model m;
    m.width = int(width);
    m.path = path;
    for (const auto &shape : layerShapes(m.width)) {
        quint32 dims[4];
        if (file.read(reinterpret_cast<char *>(dims), sizeof dims) != qint64(sizeof dims)
                || int(dims[0]) != shape[0] || int(dims[1]) != shape[1] || int(dims[2]) != shape[2]
                || dims[3] != dims[2]) {
            *error = QStringLiteral("%1: unexpected layer shape").arg(path);
            return false;
        }
        Conv conv;
        conv.out = shape[0];
        conv.in = shape[1];
        conv.kernel = shape[2];
        conv.weight.resize(std::size_t(conv.out) * conv.in * conv.kernel * conv.kernel);
        conv.bias.resize(std::size_t(conv.out));
        const qint64 wb = qint64(conv.weight.size() * sizeof(float)), bb = qint64(conv.bias.size() * sizeof(float));
        if (file.read(reinterpret_cast<char *>(conv.weight.data()), wb) != wb
                || file.read(reinterpret_cast<char *>(conv.bias.data()), bb) != bb) {
            *error = QStringLiteral("%1 is truncated").arg(path);
            return false;
        }
        for (float v : conv.weight)
            if (!std::isfinite(v)) {
                *error = QStringLiteral("%1 has invalid weights").arg(path);
                return false;
            }
        m.convs.push_back(std::move(conv));
    }
    *model = std::move(m);
    return true;
}

const Model *modelFrom(const QStringList &dirs)
{
    static std::mutex mutex;
    static std::map<QString, std::unique_ptr<Model>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    for (const QString &dir : dirs) {
        const QString path = QDir(dir).filePath(QStringLiteral("denoise.bin"));
        auto found = cache.find(path);
        if (found != cache.end()) {
            if (found->second) return found->second.get();
            continue;
        }
        std::unique_ptr<Model> model;
        if (QFileInfo::exists(path)) {
            model.reset(new Model);
            QString error;
            if (!loadModel(path, model.get(), &error)) {
                qWarning() << "mldenoise" << error;
                model.reset();
            }
        }
        const Model *result = model.get();
        cache[path] = std::move(model);
        if (result) return result;
    }
    return nullptr;
}

NoiseEstimate estimateNoise(const Tensor &planes, const std::array<double, 4> &scale,
                            const std::array<double, 4> &offset)
{
    NoiseEstimate result;
    const int w = planes.width, h = planes.height;
    std::vector<float> hp, var;
    for (int c = 0; c < 4 && c < planes.channels; ++c) {
        const float *x = planes.plane(c);
        for (int y = 1; y < h - 1; y += 4)
            for (int i = 1; i < w - 1; i += 4) {
                const float v = x[std::size_t(y) * w + i];
                hp.push_back(4 * v - x[std::size_t(y - 1) * w + i] - x[std::size_t(y + 1) * w + i]
                             - x[std::size_t(y) * w + i - 1] - x[std::size_t(y) * w + i + 1]);
                var.push_back(float(scale[c] * std::max(0.0f, v) + offset[c]));
            }
    }
    if (hp.size() < 16) return result;
    std::vector<float> a(hp.size());
    for (std::size_t i = 0; i < hp.size(); ++i) a[i] = std::abs(hp[i]);
    // numpy's median: the mean of the two middle values for an even count.
    std::vector<float> sorted = a;
    const std::size_t mid = sorted.size() / 2;
    std::nth_element(sorted.begin(), sorted.begin() + mid, sorted.end());
    double median = sorted[mid];
    if (sorted.size() % 2 == 0)
        median = 0.5 * (median + double(*std::max_element(sorted.begin(), sorted.begin() + mid)));
    double hp2 = 0, maskedVar = 0, allVar = 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < hp.size(); ++i) {
        allVar += var[i];
        if (a[i] < median) {
            hp2 += double(hp[i]) * hp[i];
            maskedVar += var[i];
            ++count;
        }
    }
    if (count == 0) return result;
    result.ratio = std::sqrt((hp2 / count) / 20.0 / std::max(maskedVar / count, 1e-20));
    result.level = result.ratio * std::sqrt(std::max(allVar / hp.size(), 1e-20));
    return result;
}

Tensor networkInput(const Tensor &planes, const std::array<double, 4> &scale, const std::array<double, 4> &offset,
                    const NoiseEstimate &noise, double gain, double *norm)
{
    const int w = planes.width, h = planes.height;
    *norm = noise.level * gain;
    const float toZ = float(gain / *norm);
    Tensor z;
    z.channels = 8;
    z.width = w;
    z.height = h;
    z.data.resize(std::size_t(8) * w * h);
    RawPipeline::parallelFor(0, 4, [&](int c) {
        const float *x = planes.plane(c);
        float *zx = z.plane(c), *zs = z.plane(4 + c);
        for (std::size_t i = 0; i < std::size_t(w) * h; ++i) zx[i] = x[i] * toZ;
        // 5x5 box mean over the valid part of the window (avg_pool2d with
        // count_include_pad=False), separable.
        std::vector<float> rows(std::size_t(w) * h);
        for (int y = 0; y < h; ++y) {
            const float *in = x + std::size_t(y) * w;
            float *out = rows.data() + std::size_t(y) * w;
            for (int i = 0; i < w; ++i) {
                float sum = 0;
                const int a = std::max(0, i - 2), b = std::min(w - 1, i + 2);
                for (int k = a; k <= b; ++k) sum += in[k];
                out[i] = sum / float(b - a + 1);
            }
        }
        const float s = float(scale[c]), o = float(offset[c]), ratio = float(noise.ratio);
        for (int y = 0; y < h; ++y) {
            const int a = std::max(0, y - 2), b = std::min(h - 1, y + 2);
            for (int i = 0; i < w; ++i) {
                float sum = 0;
                for (int k = a; k <= b; ++k) sum += rows[std::size_t(k) * w + i];
                const float mean = std::max(0.0f, sum / float(b - a + 1));
                zs[std::size_t(y) * w + i] = std::sqrt(std::max(s * mean + o, 1e-10f)) * ratio * toZ;
            }
        }
    });
    return z;
}

// ---------------------------------------------------------------- CPU path

namespace {

Tensor makeTensor(int channels, int width, int height)
{
    Tensor t;
    t.channels = channels;
    t.width = width;
    t.height = height;
    t.data.assign(std::size_t(channels) * width * height, 0.0f);
    return t;
}

// 3x3 zero-padded, 2x2 stride 2 or 1x1 convolution.
Tensor conv(const Tensor &in, const Conv &c, bool relu)
{
    const int k = c.kernel;
    const int ow = k == 2 ? in.width / 2 : in.width, oh = k == 2 ? in.height / 2 : in.height;
    Tensor out = makeTensor(c.out, ow, oh);
    RawPipeline::parallelFor(0, c.out * oh, [&](int job) {
        const int o = job / oh, y = job % oh;
        float *row = out.plane(o) + std::size_t(y) * ow;
        for (int x = 0; x < ow; ++x) row[x] = c.bias[o];
        for (int i = 0; i < c.in; ++i) {
            const float *src = in.plane(i);
            for (int ky = 0; ky < k; ++ky)
                for (int kx = 0; kx < k; ++kx) {
                    const float wgt = c.weight[((std::size_t(o) * c.in + i) * k + ky) * k + kx];
                    if (k == 2) {
                        const float *s = src + std::size_t(2 * y + ky) * in.width + kx;
                        for (int x = 0; x < ow; ++x) row[x] += wgt * s[2 * x];
                    } else {
                        const int dy = k == 3 ? ky - 1 : 0, dx = k == 3 ? kx - 1 : 0;
                        const int sy = y + dy;
                        if (sy < 0 || sy >= in.height) continue;
                        const float *s = src + std::size_t(sy) * in.width;
                        const int x0 = std::max(0, -dx), x1 = std::min(ow, in.width - dx);
                        for (int x = x0; x < x1; ++x) row[x] += wgt * s[x + dx];
                    }
                }
        }
        if (relu)
            for (int x = 0; x < ow; ++x) row[x] = std::max(0.0f, row[x]);
    });
    return out;
}

Tensor upsample(const Tensor &in)
{
    Tensor out = makeTensor(in.channels, in.width * 2, in.height * 2);
    for (int c = 0; c < in.channels; ++c)
        for (int y = 0; y < out.height; ++y)
            for (int x = 0; x < out.width; ++x)
                out.plane(c)[std::size_t(y) * out.width + x] = in.plane(c)[std::size_t(y / 2) * in.width + x / 2];
    return out;
}

Tensor concat(const Tensor &a, const Tensor &b)
{
    Tensor out = makeTensor(a.channels + b.channels, a.width, a.height);
    std::copy(a.data.begin(), a.data.end(), out.data.begin());
    std::copy(b.data.begin(), b.data.end(), out.data.begin() + a.data.size());
    return out;
}

Tensor relu(Tensor t)
{
    for (float &v : t.data) v = std::max(0.0f, v);
    return t;
}

} // namespace

Tensor runCpu(const Model &model, const Tensor &z, double norm)
{
    const std::vector<Conv> &c = model.convs;
    const Tensor e0 = conv(conv(z, c[0], true), c[1], true);
    const Tensor e1 = conv(relu(conv(e0, c[2], false)), c[3], true);
    const Tensor m = conv(conv(relu(conv(e1, c[4], false)), c[5], true), c[6], true);
    const Tensor d1 = conv(concat(upsample(conv(m, c[7], false)), e1), c[8], true);
    const Tensor d0 = conv(concat(upsample(conv(d1, c[9], false)), e0), c[10], true);
    Tensor out = conv(d0, c[11], false);
    for (int ch = 0; ch < 4; ++ch) {
        float *o = out.plane(ch);
        const float *x = z.plane(ch);
        for (std::size_t i = 0; i < std::size_t(out.width) * out.height; ++i)
            o[i] = float((x[i] + o[i]) * norm);
    }
    return out;
}

// ---------------------------------------------------------------- GPU path

namespace {

struct Input {
    int channels;
    bool upsampled; // read at half resolution (nearest), as torch's interpolate
};

QByteArray number(float v)
{
    QByteArray s = QByteArray::number(double(v), 'g', 9);
    return s;
}

// One output texel per invocation. Weights are constants; each input texel
// (4 channels) is multiplied into vec4 accumulators of 4 output channels.
// Inputs are padded by one zero texel on every side, so 3x3 taps need no
// bounds checks. With `split` every invocation computes one group of 4
// outputs (gl_GlobalInvocationID.z): less register pressure for wide layers.
QByteArray generateShader(const Conv &conv, const std::vector<Input> &inputs, bool relu, bool toBuffer, bool split)
{
    QByteArray s;
    s += "#version 310 es\nprecision mediump float;\nprecision highp int;\n";
    s += "layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;\n";
    for (std::size_t i = 0; i < inputs.size(); ++i)
        s += "layout(binding = " + QByteArray::number(int(i)) + ") uniform mediump sampler2DArray in"
                + QByteArray::number(int(i)) + ";\n";
    if (toBuffer)
        s += "layout(std430, binding = 0) writeonly buffer Out { highp vec4 result[]; };\n";
    else
        s += "layout(rgba16f, binding = 0) writeonly uniform mediump image2DArray dst;\n";
    s += "uniform ivec2 size;\n";
    s += "void main() {\n  ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n";
    s += "  if (p.x >= size.x || p.y >= size.y) return;\n";
    const int groups = conv.out / 4;
    const int k = conv.kernel;
    if (split) s += "  int og = int(gl_GlobalInvocationID.z);\n";
    s += "  mediump vec4 v;\n";
    const int blocks = split ? groups : 1;
    for (int block = 0; block < blocks; ++block) {
        const int firstGroup = split ? block : 0, lastGroup = split ? block + 1 : groups;
        if (split) s += QByteArray(block ? "  else if" : "  if") + " (og == " + QByteArray::number(block) + ") {\n";
        for (int g = firstGroup; g < lastGroup; ++g) {
            s += "  mediump vec4 a" + QByteArray::number(g) + " = vec4(";
            for (int j = 0; j < 4; ++j) s += (j ? ", " : "") + number(conv.bias[g * 4 + j]);
            s += ");\n";
        }
        int channelOffset = 0;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const Input &input = inputs[i];
            for (int ky = 0; ky < k; ++ky)
                for (int kx = 0; kx < k; ++kx) {
                    QByteArray coord;
                    if (k == 2)
                        coord = "2 * p + ivec2(" + QByteArray::number(kx + 1) + ", " + QByteArray::number(ky + 1) + ")";
                    else if (k == 1)
                        coord = "p + 1";
                    else if (input.upsampled)
                        coord = "((p + ivec2(" + QByteArray::number(kx - 1) + ", " + QByteArray::number(ky - 1)
                                + ")) >> 1) + 1";
                    else
                        coord = "p + ivec2(" + QByteArray::number(kx) + ", " + QByteArray::number(ky) + ")";
                    for (int layer = 0; layer < input.channels / 4; ++layer) {
                        s += "  v = texelFetch(in" + QByteArray::number(int(i)) + ", ivec3(" + coord + ", "
                                + QByteArray::number(layer) + "), 0);\n";
                        for (int g = firstGroup; g < lastGroup; ++g) {
                            s += "  a" + QByteArray::number(g) + " +=";
                            for (int ci = 0; ci < 4; ++ci) {
                                const int in = channelOffset + layer * 4 + ci;
                                s += QByteArray(ci ? " +" : "") + " v." + "xyzw"[ci] + " * vec4(";
                                for (int j = 0; j < 4; ++j) {
                                    const int o = g * 4 + j;
                                    s += (j ? ", " : "")
                                            + number(conv.weight[((std::size_t(o) * conv.in + in) * k + ky) * k + kx]);
                                }
                                s += ")";
                            }
                            s += ";\n";
                        }
                    }
                }
            channelOffset += input.channels;
        }
        for (int g = firstGroup; g < lastGroup; ++g) {
            const QByteArray a = "a" + QByteArray::number(g);
            const QByteArray value = relu ? "max(" + a + ", vec4(0.0))" : a;
            if (toBuffer)
                s += "  result[(" + QByteArray::number(g) + " * size.y + p.y) * size.x + p.x] = " + value + ";\n";
            else
                s += "  imageStore(dst, ivec3(p + 1, " + QByteArray::number(g) + "), " + value + ");\n";
        }
        if (split) s += "  }\n";
    }
    s += "}\n";
    return s;
}

const char *clearShader =
        "#version 310 es\n"
        "layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;\n"
        "layout(rgba16f, binding = 0) writeonly uniform mediump image2DArray dst;\n"
        "uniform ivec3 size;\n"
        "void main() {\n"
        "  ivec3 p = ivec3(gl_GlobalInvocationID);\n"
        "  if (p.x < size.x && p.y < size.y && p.z < size.z) imageStore(dst, p, vec4(0.0));\n"
        "}\n";

struct Gpu {
    std::mutex mutex;
    bool tried = false, ok = false;
    QString error;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    QByteArray identity; // renderer + version, part of the program cache key
    GLuint clear = 0;
    std::map<const Model *, std::vector<GLuint>> programs;
};

Gpu &gpu()
{
    static Gpu instance;
    return instance;
}

bool makeCurrent(Gpu &g)
{
    return eglMakeCurrent(g.display, g.surface, g.surface, g.context) == EGL_TRUE;
}

void releaseCurrent(Gpu &g)
{
    eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

QString compileError(GLuint object, bool program)
{
    char log[4096] = {0};
    if (program) glGetProgramInfoLog(object, sizeof log, nullptr, log);
    else glGetShaderInfoLog(object, sizeof log, nullptr, log);
    return QString::fromUtf8(log).trimmed();
}

// Compiles (or loads from the program binary cache) one compute program.
GLuint buildProgram(Gpu &g, const QByteArray &source, QString *error)
{
    const QByteArray key = QCryptographicHash::hash(g.identity + source, QCryptographicHash::Sha1).toHex();
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
            + QStringLiteral("/mldenoise");
    const QString cachePath = cacheDir + QLatin1Char('/') + QString::fromLatin1(key) + QStringLiteral(".bin");
    GLuint program = glCreateProgram();
    QFile cached(cachePath);
    if (cached.open(QIODevice::ReadOnly) && cached.size() > 4) {
        GLenum format = 0;
        cached.read(reinterpret_cast<char *>(&format), sizeof format);
        const QByteArray binary = cached.readAll();
        glProgramBinary(program, format, binary.constData(), GLsizei(binary.size()));
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked) return program;
        // A driver update invalidates binaries: compile again.
        glDeleteProgram(program);
        program = glCreateProgram();
    }
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    const char *text = source.constData();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        *error = QStringLiteral("Denoiser shader does not compile: %1").arg(compileError(shader, false));
        glDeleteShader(shader);
        glDeleteProgram(program);
        return 0;
    }
    glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glAttachShader(program, shader);
    glLinkProgram(program);
    glDeleteShader(shader);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        *error = QStringLiteral("Denoiser shader does not link: %1").arg(compileError(program, true));
        glDeleteProgram(program);
        return 0;
    }
    GLint length = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
    if (length > 0) {
        QByteArray binary(length, Qt::Uninitialized);
        GLenum format = 0;
        GLsizei written = 0;
        glGetProgramBinary(program, length, &written, &format, binary.data());
        if (written > 0 && QDir().mkpath(cacheDir)) {
            QSaveFile file(cachePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(reinterpret_cast<const char *>(&format), sizeof format);
                file.write(binary.constData(), written);
                file.commit();
            }
        }
    }
    return program;
}

bool initGpu(Gpu &g)
{
    if (g.tried) return g.ok;
    g.tried = true;
    g.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (g.display == EGL_NO_DISPLAY || !eglInitialize(g.display, &major, &minor)) {
        g.error = QStringLiteral("No EGL display (0x%1)").arg(eglGetError(), 0, 16);
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint configAttributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                       EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(g.display, configAttributes, &config, 1, &count) || count < 1) {
        g.error = QStringLiteral("No OpenGL ES 3 configuration");
        return false;
    }
    const EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    g.context = eglCreateContext(g.display, config, EGL_NO_CONTEXT, contextAttributes);
    if (g.context == EGL_NO_CONTEXT) {
        g.error = QStringLiteral("No OpenGL ES 3.1 context (0x%1)").arg(eglGetError(), 0, 16);
        return false;
    }
    const char *extensions = eglQueryString(g.display, EGL_EXTENSIONS);
    if (!extensions || !std::strstr(extensions, "EGL_KHR_surfaceless_context")) {
        const EGLint pbuffer[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        g.surface = eglCreatePbufferSurface(g.display, config, pbuffer);
    }
    if (!makeCurrent(g)) {
        g.error = QStringLiteral("Cannot use the OpenGL ES context (0x%1)").arg(eglGetError(), 0, 16);
        return false;
    }
    g.identity = QByteArray(reinterpret_cast<const char *>(glGetString(GL_RENDERER))) + '|'
            + QByteArray(reinterpret_cast<const char *>(glGetString(GL_VERSION)));
    GLint glMajor = 0, glMinor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &glMajor);
    glGetIntegerv(GL_MINOR_VERSION, &glMinor);
    if (glMajor * 10 + glMinor < 31) {
        g.error = QStringLiteral("OpenGL ES %1.%2 has no compute shaders").arg(glMajor).arg(glMinor);
        releaseCurrent(g);
        return false;
    }
    g.clear = buildProgram(g, clearShader, &g.error);
    releaseCurrent(g);
    g.ok = g.clear != 0;
    return g.ok;
}

GLuint texture(int width, int height, int layers)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D_ARRAY, t);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA16F, width + 2, height + 2, layers);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

// Zero padding: the layers write only the interior, so the border written
// here stays 0.
void clearTexture(GLuint clear, GLuint t, int width, int height, int layers)
{
    glUseProgram(clear);
    glUniform3i(glGetUniformLocation(clear, "size"), width + 2, height + 2, layers);
    glBindImageTexture(0, t, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(GLuint(width + 2 + 7) / 8, GLuint(height + 2 + 7) / 8, GLuint(layers));
}

struct Layer {
    int conv;
    std::vector<Input> inputs;
    bool relu;
};

// The network as run on the GPU: ReLUs that start a torch Sequential are
// applied when the previous layer stores its result.
std::vector<Layer> layers(const Model &m)
{
    const int c0 = m.width, c1 = 2 * m.width, c2 = 3 * m.width;
    return {{0, {{8, false}}, true},           {1, {{c0, false}}, true},  {2, {{c0, false}}, true},
            {3, {{c1, false}}, true},          {4, {{c1, false}}, true},  {5, {{c2, false}}, true},
            {6, {{c2, false}}, true},          {7, {{c2, false}}, false}, {8, {{c1, true}, {c1, false}}, true},
            {9, {{c1, false}}, false},         {10, {{c0, true}, {c0, false}}, true}, {11, {{c0, false}}, false}};
}

} // namespace

bool gpuAvailable(QString *error)
{
    Gpu &g = gpu();
    std::lock_guard<std::mutex> lock(g.mutex);
    const bool ok = initGpu(g);
    if (!ok && error) *error = g.error;
    return ok;
}

bool runGpu(const Model &model, const Tensor &z, double norm, Tensor *out, GpuStats *stats, QString *error)
{
    const int w = z.width, h = z.height;
    if (!model.valid() || z.channels != 8 || w % 4 || h % 4 || w < 8 || h < 8) {
        *error = QStringLiteral("Denoiser input must be 8 channels, size a multiple of 4");
        return false;
    }
    Gpu &g = gpu();
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!initGpu(g)) {
        *error = g.error;
        return false;
    }
    if (!makeCurrent(g)) {
        *error = QStringLiteral("Cannot use the OpenGL ES context (0x%1)").arg(eglGetError(), 0, 16);
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    const std::vector<Layer> net = layers(model);
    std::vector<GLuint> &programs = g.programs[&model];
    if (programs.empty()) {
        for (const Layer &layer : net) {
            const Conv &conv = model.convs[std::size_t(layer.conv)];
            const bool last = layer.conv == 11;
            const GLuint program = buildProgram(g, generateShader(conv, layer.inputs, layer.relu, last, conv.out >= 16),
                                                error);
            if (!program) {
                for (GLuint p : programs) glDeleteProgram(p);
                programs.clear();
                releaseCurrent(g);
                return false;
            }
            programs.push_back(program);
        }
    }
    if (stats) stats->compileMs = timer.restart();

    // Activations, reused once dead (see layers()): full resolution A (z,
    // later d0), B (e0a), C (e0); half D (d, later d1), E (e1, later u1);
    // quarter F (m0, later m), G (ma, later u2).
    const int c0 = model.width, c1 = 2 * model.width, c2 = 3 * model.width;
    const int w2 = w / 2, h2 = h / 2, w4 = w / 4, h4 = h / 4;
    struct Slot {
        int width, height, layers;
        GLuint texture;
    };
    Slot stores[7] = {{w, h, std::max(2, c0 / 4), 0}, {w, h, c0 / 4, 0},  {w, h, c0 / 4, 0},
                     {w2, h2, c1 / 4, 0},            {w2, h2, c1 / 4, 0}, {w4, h4, c2 / 4, 0},
                     {w4, h4, c2 / 4, 0}};
    for (Slot &slot : stores) {
        slot.texture = texture(slot.width, slot.height, slot.layers);
        clearTexture(g.clear, slot.texture, slot.width, slot.height, slot.layers);
    }
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);

    // Upload z into slot A's interior, 4 channels per layer, as half floats.
    {
        std::vector<_Float16> layer(std::size_t(w) * h * 4);
        glBindTexture(GL_TEXTURE_2D_ARRAY, stores[0].texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (int l = 0; l < 2; ++l) {
            const float *planes[4] = {z.plane(l * 4), z.plane(l * 4 + 1), z.plane(l * 4 + 2), z.plane(l * 4 + 3)};
            RawPipeline::parallelFor(0, h, [&](int y) {
                _Float16 *row = layer.data() + std::size_t(y) * w * 4;
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 4; ++c) row[x * 4 + c] = _Float16(planes[c][std::size_t(y) * w + x]);
            });
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 1, 1, l, w, h, 1, GL_RGBA, GL_HALF_FLOAT, layer.data());
        }
    }
    GLuint result = 0;
    glGenBuffers(1, &result);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, result);
    glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(std::size_t(w) * h * 4 * sizeof(float)), nullptr,
                 GL_STREAM_READ);
    glFinish();
    if (stats) stats->uploadMs = timer.restart();

    // (inputs, output) slot per layer; -1: the result buffer.
    const int io[12][3] = {{0, -1, 1}, {1, -1, 2}, {2, -1, 3}, {3, -1, 4}, {4, -1, 5}, {5, -1, 6},
                           {6, -1, 5}, {5, -1, 6}, {6, 4, 3}, {3, -1, 4}, {4, 2, 0}, {0, -1, -1}};
    for (std::size_t i = 0; i < net.size(); ++i) {
        const Conv &conv = model.convs[std::size_t(net[i].conv)];
        const int outSlot = io[i][2];
        const int ow = conv.kernel == 2 ? stores[io[i][0]].width / 2 : (outSlot >= 0 ? stores[outSlot].width : w);
        const int oh = conv.kernel == 2 ? stores[io[i][0]].height / 2 : (outSlot >= 0 ? stores[outSlot].height : h);
        glUseProgram(programs[i]);
        glUniform2i(glGetUniformLocation(programs[i], "size"), ow, oh);
        for (int input = 0; input < 2; ++input) {
            if (io[i][input] < 0) continue;
            glActiveTexture(GLenum(GL_TEXTURE0 + input));
            glBindTexture(GL_TEXTURE_2D_ARRAY, stores[io[i][input]].texture);
        }
        if (outSlot >= 0)
            glBindImageTexture(0, stores[outSlot].texture, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        else
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, result);
        const bool split = conv.out >= 16;
        glDispatchCompute(GLuint(ow + 7) / 8, GLuint(oh + 7) / 8, split ? GLuint(conv.out / 4) : 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT
                        | GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    }
    glFinish();
    if (stats) stats->computeMs = timer.restart();

    bool ok = true;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, result);
    const auto *data = static_cast<const float *>(
            glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, GLsizeiptr(std::size_t(w) * h * 4 * sizeof(float)),
                             GL_MAP_READ_BIT));
    if (!data) {
        *error = QStringLiteral("Cannot read the denoiser result (0x%1)").arg(glGetError(), 0, 16);
        ok = false;
    } else {
        *out = makeTensor(4, w, h);
        RawPipeline::parallelFor(0, 4, [&](int c) {
            float *o = out->plane(c);
            const float *x = z.plane(c);
            for (std::size_t i = 0; i < std::size_t(w) * h; ++i)
                o[i] = float((x[i] + data[i * 4 + std::size_t(c)]) * norm);
        });
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    }
    const GLenum glError = glGetError();
    if (ok && glError != GL_NO_ERROR) {
        *error = QStringLiteral("OpenGL error 0x%1 in the denoiser").arg(glError, 0, 16);
        ok = false;
    }
    glDeleteBuffers(1, &result);
    for (Slot &slot : stores) glDeleteTextures(1, &slot.texture);
    glFinish();
    releaseCurrent(g);
    if (stats) stats->downloadMs = timer.restart();
    return ok;
}

bool denoisePlanes(const Model &model, Tensor *planes, const std::array<double, 4> &scale,
                   const std::array<double, 4> &offset, double gain, QJsonObject *info, QString *error)
{
    QElapsedTimer timer;
    timer.start();
    if (!(gain > 0) || !std::isfinite(gain)) gain = 1.0;
    // The network needs sizes divisible by 4: denoise the largest such
    // area, the rest (at most 3 rows and columns) stays as it is.
    const int w = planes->width & ~3, h = planes->height & ~3;
    Tensor crop = *planes;
    if (w != planes->width || h != planes->height) {
        crop = makeTensor(4, w, h);
        for (int c = 0; c < 4; ++c)
            for (int y = 0; y < h; ++y)
                std::memcpy(crop.plane(c) + std::size_t(y) * w, planes->plane(c) + std::size_t(y) * planes->width,
                            sizeof(float) * std::size_t(w));
    }
    const NoiseEstimate noise = estimateNoise(crop, scale, offset);
    if (!(noise.level > 0) || !std::isfinite(noise.level)) {
        *error = QStringLiteral("Cannot estimate the noise level");
        return false;
    }
    double norm = 1;
    const Tensor z = networkInput(crop, scale, offset, noise, gain, &norm);
    const qint64 prepareMs = timer.restart();
    Tensor result;
    GpuStats stats;
    if (!runGpu(model, z, norm, &result, &stats, error)) return false;
    const float toPlanes = float(1.0 / gain);
    for (int c = 0; c < 4; ++c)
        for (int y = 0; y < h; ++y) {
            float *dst = planes->plane(c) + std::size_t(y) * planes->width;
            const float *src = result.plane(c) + std::size_t(y) * w;
            for (int x = 0; x < w; ++x) dst[x] = src[x] * toPlanes;
        }
    if (info) {
        info->insert(QStringLiteral("model"), QFileInfo(model.path).fileName());
        info->insert(QStringLiteral("width"), model.width);
        info->insert(QStringLiteral("noise_ratio"), noise.ratio);
        info->insert(QStringLiteral("noise_level"), noise.level);
        info->insert(QStringLiteral("gain"), gain);
        info->insert(QStringLiteral("prepare_ms"), double(prepareMs));
        info->insert(QStringLiteral("compile_ms"), stats.compileMs);
        info->insert(QStringLiteral("upload_ms"), stats.uploadMs);
        info->insert(QStringLiteral("compute_ms"), stats.computeMs);
        info->insert(QStringLiteral("download_ms"), stats.downloadMs + timer.elapsed());
    }
    return true;
}

}
