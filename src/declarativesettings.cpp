// SPDX-FileCopyrightText: 2013 - 2024 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include "declarativesettings.h"
#include "storagefolders.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QLocale>
#include <QTemporaryFile>
#include <partitionmanager.h>

#include <pwd.h>
#include <unistd.h>
#include <sys/types.h>
#include <limits>

DeclarativeSettings::DeclarativeSettings(QObject *parent)
    : QObject(parent)
    , m_partitionManager(new PartitionManager(this))
    , m_storagePath(QStringLiteral("/apps/procam/storagePath"))
    , m_minSpaceForRecording(QStringLiteral("/apps/procam/minSpaceForRecording"))
    , m_customPhotoDirectory(QStringLiteral("/apps/procam/photoDirectory"))
    , m_customVideoDirectory(QStringLiteral("/apps/procam/videoDirectory"))
    , m_customPhotoDirectoryAvailable(false)
    , m_customVideoDirectoryAvailable(false)
    , m_storagePathStatus(NotSet)
    , m_storageMaxFileSize(0)
{
    connect(&m_storagePath, SIGNAL(valueChanged()), this, SLOT(verifyStoragePath()));
    connect(&m_customPhotoDirectory, SIGNAL(valueChanged()), this, SIGNAL(customPhotoDirectoryChanged()));
    connect(&m_customPhotoDirectory, SIGNAL(valueChanged()), this, SLOT(verifyStoragePath()));
    connect(&m_customVideoDirectory, SIGNAL(valueChanged()), this, SIGNAL(customVideoDirectoryChanged()));
    connect(&m_customVideoDirectory, SIGNAL(valueChanged()), this, SLOT(verifyStoragePath()));
    connect(m_partitionManager, SIGNAL(partitionRemoved(const Partition&)), this, SLOT(verifyStoragePath()));
    connect(m_partitionManager, SIGNAL(partitionAdded(const Partition&)), this, SLOT(verifyStoragePath()));
    connect(m_partitionManager, SIGNAL(partitionChanged(const Partition&)), this, SLOT(verifyStoragePath()));

    verifyStoragePath();

    // protect against camera crashes leaving files in the hidden directory
    for (const QFileInfo &info : QDir(videoDirectory() + QLatin1String("/.recording")).entryInfoList(QDir::Files)) {
        QString targetPath = videoDirectory() + "/" + info.fileName();
        QFile(info.absoluteFilePath()).rename(targetPath);
        fixupPermissions(targetPath);
    }
}

DeclarativeSettings::~DeclarativeSettings()
{
}

QObject *DeclarativeSettings::factory(QQmlEngine *engine, QJSEngine *)
{
    const QUrl source = QUrl::fromLocalFile(QStringLiteral(DEPLOYMENT_PATH "settings.qml"));
    QQmlComponent component(engine, source);
    if (component.isReady()) {
        return component.create();
    } else {
        qWarning() << "Failed to instantiate Settings";
        qWarning() << component.errors();

        return nullptr;
    }
}

QString DeclarativeSettings::photoDirectory() const
{
    return m_photoDirectory;
}

QString DeclarativeSettings::videoDirectory() const
{
    return m_videoDirectory;
}

QString DeclarativeSettings::storagePath() const
{
    return m_storagePath.value().toString();
}

void DeclarativeSettings::setStoragePath(const QString &path)
{
    if (path == storagePath())
        return;

    if (path.isEmpty()) {
        m_storagePath.unset();
        m_storagePathStatus = NotSet;
    } else {
        m_storagePath.set(path);
    }

    // notifiers will be handled by the MDConfItem change signal connection
}

DeclarativeSettings::StoragePathStatus DeclarativeSettings::storagePathStatus() const
{
    return m_storagePathStatus;
}

qint64 DeclarativeSettings::storageMaxFileSize() const
{
    return m_storageMaxFileSize;
}

QString DeclarativeSettings::customPhotoDirectory() const
{
    return m_customPhotoDirectory.value().toString();
}

void DeclarativeSettings::setCustomPhotoDirectory(const QString &path)
{
    if (path == customPhotoDirectory())
        return;

    if (path.isEmpty())
        m_customPhotoDirectory.unset();
    else
        m_customPhotoDirectory.set(QDir::cleanPath(path));
}

QString DeclarativeSettings::customVideoDirectory() const
{
    return m_customVideoDirectory.value().toString();
}

void DeclarativeSettings::setCustomVideoDirectory(const QString &path)
{
    if (path == customVideoDirectory())
        return;

    if (path.isEmpty())
        m_customVideoDirectory.unset();
    else
        m_customVideoDirectory.set(QDir::cleanPath(path));
}

bool DeclarativeSettings::customPhotoDirectoryAvailable() const
{
    return m_customPhotoDirectoryAvailable;
}

bool DeclarativeSettings::customVideoDirectoryAvailable() const
{
    return m_customVideoDirectoryAvailable;
}

bool DeclarativeSettings::folderAllowed(const QString &path) const
{
    const struct passwd *account = getpwuid(getuid());
    const QStringList roots = procamFolderRoots(
                QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
                QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
                account ? QString::fromLocal8Bit(account->pw_name) : QString());
    return procamFolderAllowed(path, roots);
}

// A picked folder is used only while it exists and can be written to: a
// memory card that is out, or a folder deleted in the meantime, falls back to
// the default folder instead of recreating the path somewhere unexpected.
bool DeclarativeSettings::customDirectoryUsable(const QString &path)
{
    return folderAllowed(path) && QFileInfo(path).isDir() && verifyWritable(path);
}

void DeclarativeSettings::fixupPermissions(const QString &targetPath)
{
    const QByteArray path = targetPath.toUtf8();
    if (chown(path.constData(), getuid(), getgid()) != 0)
         qWarning() << "Could not change owner/group of resulting photo capture file:" << targetPath << strerror(errno);
}

bool DeclarativeSettings::verifyWritable(const QString &path)
{
    QTemporaryFile file(path + QStringLiteral("/XXXXXX.tmp"));
    file.setAutoRemove(true);
    return file.open();
}

static qint64 getMaxBytes(Partition partition)
{
    const qint64 vfatMaxFileSize = std::numeric_limits<quint32>::max();
    qint64 realMaxBytes = partition.bytesAvailable();
    if (partition.filesystemType() == "vfat" && partition.bytesAvailable() > vfatMaxFileSize)
        realMaxBytes = vfatMaxFileSize;

    return realMaxBytes;
}

// The recording limit for a picked video folder, as for the storage choice:
// a memory card may be filled, device memory keeps minSpaceForRecording free.
qint64 DeclarativeSettings::maxFileSizeIn(const QString &directory) const
{
    const QString rootPath = QStorageInfo(directory).rootPath();
    const QVector<Partition> partitions = m_partitionManager->partitions();
    auto it = std::find_if(partitions.begin(), partitions.end(), [rootPath](const Partition &partition) {
        return partition.mountPath() == rootPath; });
    if (it == partitions.end())
        return 0;

    if (it->storageType() == Partition::External)
        return getMaxBytes(*it);
    return qMax((qint64)0, getMaxBytes(*it) - (m_minSpaceForRecording.value(100).toLongLong() << 20));
}

void DeclarativeSettings::verifyStoragePath()
{
    const QString prevPhotoPath = m_photoDirectory;
    const QString prevVideoPath = m_videoDirectory;
    const bool prevPhotoAvailable = m_customPhotoDirectoryAvailable;
    const bool prevVideoAvailable = m_customVideoDirectoryAvailable;

    QString path = storagePath();
    StoragePathStatus oldStatus = m_storagePathStatus;
    qint64 oldMaxBytes = m_storageMaxFileSize;

    m_storagePathStatus = path.isEmpty() ? NotSet : Unavailable;

    if (!path.isEmpty()) {
        QVector<Partition> partitions = m_partitionManager->partitions(Partition::External | Partition::ExcludeParents);
        auto it = std::find_if(partitions.begin(), partitions.end(),
                               [path](const Partition &partition) { return partition.mountPath() == path; });
        if (it != partitions.end()) {
            const Partition &partition = *it;
            if (partition.status() == Partition::Mounted) {
                if (verifyWritable(storagePath())) {
                    m_storagePathStatus = Available;
                    m_storageMaxFileSize = getMaxBytes(partition);
                } else {
                    m_storagePathStatus = Unavailable;
                    m_storageMaxFileSize = 0;
                }
            } else if(partition.status() == Partition::Mounting) {
                m_storagePathStatus = Mounting;
                m_storageMaxFileSize = 0;
            }
        }
    }

    if (m_storagePathStatus == Available && !path.isEmpty()) {
        m_photoDirectory = path + QStringLiteral("/Pictures/ProCam");
        m_videoDirectory = path + QStringLiteral("/Videos/ProCam");
    } else {
        m_photoDirectory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + QLatin1String("/ProCam");
        m_videoDirectory = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + QLatin1String("/ProCam");
        QVector<Partition> partitions = m_partitionManager->partitions(Partition::User | Partition::Mass);
        auto it = std::find_if(partitions.begin(), partitions.end(), [path](const Partition &partition) {
            return QStandardPaths::writableLocation(QStandardPaths::MoviesLocation).startsWith(partition.mountPath()); });
        if (it != partitions.end()) {
            const Partition &partition = *it;
            m_storageMaxFileSize = qMax((qint64)0,
                                        getMaxBytes(partition) - (m_minSpaceForRecording.value(100).toLongLong() << 20));
        } else {
            m_storageMaxFileSize = 0;
        }
    }

    const QString customPhotoPath = customPhotoDirectory();
    m_customPhotoDirectoryAvailable = !customPhotoPath.isEmpty() && customDirectoryUsable(customPhotoPath);
    if (m_customPhotoDirectoryAvailable)
        m_photoDirectory = customPhotoPath;

    const QString customVideoPath = customVideoDirectory();
    m_customVideoDirectoryAvailable = !customVideoPath.isEmpty() && customDirectoryUsable(customVideoPath);
    if (m_customVideoDirectoryAvailable) {
        m_videoDirectory = customVideoPath;
        m_storageMaxFileSize = maxFileSizeIn(customVideoPath);
    }

    QDir(m_photoDirectory).mkpath(QLatin1String("."));
    QDir(m_videoDirectory).mkpath(QLatin1String(".recording"));

    if (prevPhotoPath != m_photoDirectory || prevPhotoAvailable != m_customPhotoDirectoryAvailable) {
        emit photoDirectoryChanged();
    }
    if (prevVideoPath != m_videoDirectory || prevVideoAvailable != m_customVideoDirectoryAvailable) {
        emit videoDirectoryChanged();
    }

    if (oldStatus != m_storagePathStatus) {
        emit storagePathStatusChanged();
    }

    if (oldMaxBytes != m_storageMaxFileSize) {
        emit storageMaxFileSizeChanged();
    }
}

QString DeclarativeSettings::photoCapturePath(const QString &extension)
{
    verifyCapturePrefix();

    QString fileFormat(photoDirectory() + QLatin1Char('/') + m_prefix + QLatin1String("%1.") + extension);
    return capturePath(fileFormat);
}

QString DeclarativeSettings::videoCapturePath(const QString &extension)
{
    verifyCapturePrefix();

    QString fileFormat(videoDirectory() + QLatin1String("/.recording/") + m_prefix + QLatin1String("%1.") + extension);
    return capturePath(fileFormat);
}

void DeclarativeSettings::completePhoto(const QUrl &file)
{
    fixupPermissions(file.path());
}

void DeclarativeSettings::refreshMaxFileSize()
{
    m_partitionManager->refresh();
}

QUrl DeclarativeSettings::completeCapture(const QUrl &file)
{
    const QString recordingDir = QStringLiteral("/.recording/");
    const QString absolutePath = file.toLocalFile();
    const int index = absolutePath.lastIndexOf(recordingDir) + 1;
    if (index == -1) {
        return file;
    }

    QString targetPath = absolutePath;
    targetPath.remove(index, recordingDir.length() - 1);

    if (QFile::rename(absolutePath, targetPath)) {
        fixupPermissions(targetPath);
        refreshMaxFileSize();
        return QUrl::fromLocalFile(targetPath);
    } else {
        QFile::remove(absolutePath);
        return QUrl();
    }
}

void DeclarativeSettings::verifyCapturePrefix()
{
    const QDateTime currentDate = QDateTime::currentDateTime();
    if (m_prefixDate != currentDate) {
        m_prefixDate = currentDate;
        m_prefix = QLatin1String("ProCam_")
                + QLocale::c().toString(currentDate, QLatin1String("yyyyMMdd_HHmmss"));
    }
}

QString DeclarativeSettings::capturePath(const QString &format)
{
    QString path = format.arg(QString(""));
    if (!QFile::exists(path)) {
        return path;
    }

    int counter = 1;
    for (;;) {
        path = format.arg(QString(QStringLiteral("_%1")).arg(counter, 3, 10, QLatin1Char('0')));
        if (!QFile::exists(path))
            return path;
        ++counter;
    }
}
