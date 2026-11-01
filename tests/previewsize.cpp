#include "../src/previewsize.h"
#include <algorithm>
#undef NDEBUG
#include <cassert>

int main()
{
    auto select = [](const QStringList &sizes) {
        return preferredPreviewSize(sizes, sizes);
    };
    QStringList xperia = {"3264x2448", "1920x1440", "1920x1080",
                          "1440x1080", "1280x720", "640x480", "320x240"};
    assert(select(xperia) == QSize(1440, 1080));
    std::reverse(xperia.begin(), xperia.end());
    assert(select(xperia) == QSize(1440, 1080));
    QStringList existing = xperia;
    existing << "1280x960";
    assert(select(existing) == QSize(1280, 960));
    assert(select({"1920x1080", "1280x720"}) == QSize(1280, 720));
    assert(select({"4000x3000", "1920x1080"}) == QSize(1920, 1080));
    assert(select({"640x480", "640x480", "bad", "0x480", "-1x3"}) == QSize(640, 480));
    assert(!select({}).isValid());
    assert(!select({"0x0", "bad", "999999999999x480"}).isValid());
    assert(!preferredPreviewSize(xperia, {}).isValid());
    assert(!preferredPreviewSize({}, {"1280x960"}).isValid());
    assert(preferredPreviewSize(xperia, {"1280x960", "640x480"}) == QSize(640, 480));
    // Re-evaluating for another camera must not retain the previous choice.
    assert(select({"640x480"}) == QSize(640, 480));
    assert(select({"1280x720"}) == QSize(1280, 720));
    // The YUV preview transport asks for 1440x1080 (within Camera2's
    // guaranteed PREVIEW size next to a RAW stream).
    const QSize larger(1440, 1080);
    const QStringList jp2 = {"1920x1440", "1600x1200", "1440x1080", "1280x960", "1280x720"};
    assert(preferredPreviewSize(jp2, jp2, larger) == larger);
    assert(preferredPreviewSize(existing, existing, larger) == larger);
    assert(preferredPreviewSize({"1280x960", "640x480"}, {"1280x960", "640x480"}, larger) == QSize(1280, 960));
}
