// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// Qt image format plugin for HEIC/HEIF (read only), with libheif and
// libde265 linked in statically. Sailfish OS has no HEIF support of its own:
// this lets the Gallery, thumbnails and ProCam show ProCam's HEIC photos
// (and HEIC files from elsewhere). The primary image is decoded with its
// rotation and mirroring applied; the size comes from the file's headers
// without decoding.

#include <QImageIOHandler>
#include <QImageIOPlugin>
#include <QImage>
#include <QSize>
#include <QVariant>

#include <libheif/heif.h>

#include <cstring>
#include <memory>

namespace {

// ftyp brands of HEIF files with HEVC images (libde265 is the only decoder
// built in, so AVIF is left to others).
bool isHeif(QIODevice *device)
{
    if (!device) return false;
    const QByteArray header = device->peek(32);
    if (header.size() < 12 || header.mid(4, 4) != "ftyp") return false;
    static const char *const brands[] = { "heic", "heix", "heim", "heis", "hevc",
                                          "hevx", "mif1", "msf1" };
    for (int offset = 8; offset + 4 <= header.size(); offset += 4) {
        if (offset == 12) continue; // minor version
        for (const char *brand : brands) {
            if (header.mid(offset, 4) == brand) return true;
        }
    }
    return false;
}

struct ContextDeleter { void operator()(heif_context *c) const { heif_context_free(c); } };
struct HandleDeleter { void operator()(heif_image_handle *h) const { heif_image_handle_release(h); } };
struct ImageDeleter { void operator()(heif_image *i) const { heif_image_release(i); } };

class HeifHandler : public QImageIOHandler
{
public:
    bool canRead() const override
    {
        if (isHeif(device())) {
            setFormat("heic");
            return true;
        }
        return false;
    }

    bool read(QImage *image) override
    {
        if (!load()) return false;
        heif_decoding_options *options = heif_decoding_options_alloc();
        heif_image *decoded = nullptr;
        const bool alpha = heif_image_handle_has_alpha_channel(m_handle.get());
        const heif_error error = heif_decode_image(
                    m_handle.get(), &decoded, heif_colorspace_RGB,
                    alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB,
                    options);
        heif_decoding_options_free(options);
        if (error.code != heif_error_Ok || !decoded) return false;
        std::unique_ptr<heif_image, ImageDeleter> owned(decoded);

        const int width = heif_image_get_width(decoded, heif_channel_interleaved);
        const int height = heif_image_get_height(decoded, heif_channel_interleaved);
        int stride = 0;
        const uint8_t *pixels = heif_image_get_plane_readonly(decoded, heif_channel_interleaved,
                                                              &stride);
        if (!pixels || width <= 0 || height <= 0) return false;
        QImage result(width, height, alpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
        if (result.isNull()) return false;
        const int rowBytes = width * (alpha ? 4 : 3);
        for (int y = 0; y < height; ++y)
            std::memcpy(result.scanLine(y), pixels + size_t(y) * size_t(stride), size_t(rowBytes));
        // Thumbnails ask for a smaller image: decode, then scale (HEVC has
        // no cheap reduced decode).
        if (m_scaledSize.isValid() && m_scaledSize != result.size())
            result = result.scaled(m_scaledSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        *image = result;
        return true;
    }

    bool supportsOption(ImageOption option) const override
    {
        return option == Size || option == ScaledSize;
    }

    QVariant option(ImageOption option) const override
    {
        if (option == Size && const_cast<HeifHandler *>(this)->load()) {
            // The size with rotation applied, as read() returns it.
            return QSize(heif_image_handle_get_width(m_handle.get()),
                         heif_image_handle_get_height(m_handle.get()));
        }
        if (option == ScaledSize) return m_scaledSize;
        return QVariant();
    }

    void setOption(ImageOption option, const QVariant &value) override
    {
        if (option == ScaledSize) m_scaledSize = value.toSize();
    }

private:
    bool load()
    {
        if (m_handle) return true;
        if (m_failed || !device()) return false;
        m_failed = true;
        m_data = device()->readAll();
        if (m_data.isEmpty()) return false;
        m_context.reset(heif_context_alloc());
        if (!m_context) return false;
        heif_error error = heif_context_read_from_memory_without_copy(
                    m_context.get(), m_data.constData(), size_t(m_data.size()), nullptr);
        if (error.code != heif_error_Ok) return false;
        heif_image_handle *handle = nullptr;
        error = heif_context_get_primary_image_handle(m_context.get(), &handle);
        if (error.code != heif_error_Ok || !handle) return false;
        m_handle.reset(handle);
        m_failed = false;
        return true;
    }

    QByteArray m_data;
    std::unique_ptr<heif_context, ContextDeleter> m_context;
    std::unique_ptr<heif_image_handle, HandleDeleter> m_handle;
    QSize m_scaledSize;
    bool m_failed = false;
};

} // namespace

class HeifPlugin : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "heif.json")

public:
    HeifPlugin()
    {
        heif_init(nullptr);
    }

    Capabilities capabilities(QIODevice *device, const QByteArray &format) const override
    {
        if (format == "heic" || format == "heif" || format == "hif") return CanRead;
        if (!format.isEmpty() || !device || !device->isOpen()) return {};
        return isHeif(device) ? CanRead : Capabilities();
    }

    QImageIOHandler *create(QIODevice *device, const QByteArray &format) const override
    {
        auto *handler = new HeifHandler;
        handler->setDevice(device);
        handler->setFormat(format.isEmpty() ? QByteArray("heic") : format);
        return handler;
    }
};

#include "heifplugin.moc"
