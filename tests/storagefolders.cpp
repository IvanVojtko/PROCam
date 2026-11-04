#include "../src/storagefolders.h"
#undef NDEBUG
#include <cassert>

int main()
{
    const QStringList roots = procamFolderRoots(
                QStringLiteral("/home/defaultuser/Pictures"),
                QStringLiteral("/home/defaultuser/Videos/"),
                QStringLiteral("defaultuser"));
    assert(roots.size() == 3);
    assert(roots.at(1) == QStringLiteral("/home/defaultuser/Videos"));

    // The roots themselves and anything below them.
    assert(procamFolderAllowed(QStringLiteral("/home/defaultuser/Pictures"), roots));
    assert(procamFolderAllowed(QStringLiteral("/home/defaultuser/Pictures/Trips/2026"), roots));
    assert(procamFolderAllowed(QStringLiteral("/home/defaultuser/Videos/ProCam/"), roots));
    assert(procamFolderAllowed(QStringLiteral("/run/media/defaultuser/sdcard/DCIM"), roots));

    // Outside the sandbox, or only looking like a root.
    assert(!procamFolderAllowed(QStringLiteral("/home/defaultuser/Documents"), roots));
    assert(!procamFolderAllowed(QStringLiteral("/home/defaultuser/PicturesOld"), roots));
    assert(!procamFolderAllowed(QStringLiteral("/home/defaultuser/Pictures/../Documents"), roots));
    assert(!procamFolderAllowed(QStringLiteral("/run/media/otheruser/sdcard"), roots));
    assert(!procamFolderAllowed(QStringLiteral("/run/media"), roots));
    assert(!procamFolderAllowed(QStringLiteral("Pictures/ProCam"), roots));
    assert(!procamFolderAllowed(QString(), roots));

    // No user known: no memory cards, the home folders still work.
    const QStringList noUser = procamFolderRoots(QStringLiteral("/home/u/Pictures"),
                                                 QStringLiteral("/home/u/Videos"), QString());
    assert(noUser.size() == 2);
    assert(!procamFolderAllowed(QStringLiteral("/run/media/u/card"), noUser));
    return 0;
}
