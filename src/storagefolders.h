#ifndef STORAGEFOLDERS_H
#define STORAGEFOLDERS_H

#include <QDir>
#include <QString>
#include <QStringList>

// Folders the user may pick for photos and videos. ProCam runs under
// Sailjail with the Pictures, Videos and RemovableMedia permissions, so it
// can write only below those roots; a folder elsewhere would look fine in the
// settings page (which is not sandboxed) and fail in the camera.
inline QStringList procamFolderRoots(const QString &pictures, const QString &videos,
                                     const QString &user)
{
    QStringList roots;
    if (!pictures.isEmpty())
        roots.append(QDir::cleanPath(pictures));
    if (!videos.isEmpty())
        roots.append(QDir::cleanPath(videos));
    if (!user.isEmpty())
        roots.append(QStringLiteral("/run/media/") + user);
    return roots;
}

// The root itself counts, so ~/Pictures can be chosen. Paths are compared
// after cleanPath, so "~/Pictures/../Documents" is not let through.
inline bool procamFolderAllowed(const QString &path, const QStringList &roots)
{
    if (path.isEmpty() || !QDir::isAbsolutePath(path))
        return false;
    const QString clean = QDir::cleanPath(path);
    for (const QString &root : roots) {
        if (clean == root || clean.startsWith(root + QLatin1Char('/')))
            return true;
    }
    return false;
}

#endif
