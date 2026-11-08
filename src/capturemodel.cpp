// SPDX-FileCopyrightText: 2013 - 2021 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include "capturemodel.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QMimeType>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QRunnable>
#include <QThreadPool>
#include <QUrl>

#include <dirent.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace  {

template <class Function>
class AsyncFunction : public QRunnable
{
public:
    explicit AsyncFunction(const Function &function)
        : m_function(function)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        m_function();
    }

private:
    Function m_function;
};

template <class Function> void runAsync(const Function &function)
{
    QThreadPool::globalInstance()->start(new AsyncFunction<Function>(function));
}

QByteArray cameraSortName(const QByteArray &fileName)
{
    const QByteArray prefix = QByteArrayLiteral("ProCam_");
    return fileName.startsWith(prefix) ? fileName.mid(prefix.size()) : fileName;
}

class InvokableEvent : public QEvent
{
public:
    InvokableEvent()
        : QEvent(User)
    {
    }

    virtual void invoke() = 0;
};

template <class Function>
class FunctionEvent : public InvokableEvent
{
public:
    explicit FunctionEvent(const Function &function)
        : m_function(function)
    {
    }

    ~FunctionEvent()
    {
    }

    void invoke() override
    {
        m_function();
    }

private:
    Function m_function;
};

}

QString CaptureModel::Capture::filePath() const
{
    return QString::fromUtf8(directory + '/' + fileName);
}

CaptureModel::CaptureModel(QObject *parent)
    : QAbstractListModel(parent)
{
    connect(&m_notifier, &QSocketNotifier::activated, this, &CaptureModel::filesChanged);
}

CaptureModel::~CaptureModel()
{
    close(m_inotifyFd);

    QMutexLocker locker(&m_exitMutex);

    if (m_scanning) {
        m_exitCondition.wait(&m_exitMutex);
    }
}

bool CaptureModel::isPopulated() const
{
    return m_populated;
}

QStringList CaptureModel::directories() const
{
    return m_directories;
}

void CaptureModel::setDirectories(const QStringList &directories)
{
    if (m_directories != directories) {
        m_directories = directories;

        emit directoriesChanged();
    }

    updateWatchedDirectories();
}

void CaptureModel::appendCapture(const QUrl &url, const QString &mimeType)
{
    if (m_notifier.isEnabled()) {
        const QByteArray filePath = url.toLocalFile().toUtf8();

        const int index = filePath.lastIndexOf('/');
        if (index != -1) {
            const QByteArray directoryPath = filePath.mid(0, index);
            const QByteArray fileName = filePath.mid(index + 1);

            for (const WatchedDirectory &directory : m_watchedDirectories) {
                if (directory.path == directoryPath) {
                    insertCapture(directory, fileName, mimeType);

                    return;
                }
            }

            // The new file belongs to a directory that's not being watched and may therefore
            // be newly created.
            updateWatchedDirectories();
        }
    }
}

void CaptureModel::deleteFile(int index)
{
    if (index >= 0 && index < count()) {
        const Capture &capture = captureAt(index);

        QFile::remove(capture.filePath());

        if (m_notifier.isEnabled()) {
            beginRemoveRows(QModelIndex(), index, index);
            m_captures.removeAt(index);
            m_maximumCaptureIndex -= 1;
            endRemoveRows();

            emit countChanged();
        }
    }
}

QHash<int, QByteArray> CaptureModel::roleNames() const
{
    static const QHash<int, QByteArray> roleNames = {
        { Url, "url" },
        { MimeType, "mimeType" }
    };
    return roleNames;
}

QModelIndex CaptureModel::index(int row, int column, const QModelIndex &parent) const
{
    return !parent.isValid() && row >= 0 && row < count() && column == 0
                ? createIndex(row, column)
                : QModelIndex();
}

int CaptureModel::rowCount(const QModelIndex &parent) const
{
    return !parent.isValid() ? count() : 0;
}

QVariant CaptureModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    } else {
        const Capture &capture = captureAt(index.row());
        switch (role) {
        case Url:
            return QUrl::fromLocalFile(capture.filePath());
        case MimeType:
            if (capture.mimeType.isEmpty()) {
                const_cast<Capture &>(capture).mimeType = m_mimeDatabase.mimeTypeForFile(
                            capture.filePath()).name();
            }
            return capture.mimeType;
        }
    }
    return QVariant();
}

bool CaptureModel::event(QEvent *event)
{
    if (event->type() == QEvent::User) {
        static_cast<InvokableEvent *>(event)->invoke();

        return true;
    } else {
        return QAbstractListModel::event(event);
    }
}

void CaptureModel::classBegin()
{
    m_complete = false;
}

void CaptureModel::componentComplete()
{
    m_complete = true;

    updateWatchedDirectories();
}

int CaptureModel::count() const
{
    return m_maximumCaptureIndex + m_expiredCaptures.count() - m_minimumExpiredIndex;
}

const CaptureModel::Capture &CaptureModel::captureAt(int index) const
{
    return index < m_maximumCaptureIndex
            ? m_captures.at(index)
            : m_expiredCaptures.at(index - m_maximumCaptureIndex + m_minimumExpiredIndex);
}

void CaptureModel::updateWatchedDirectories()
{
    if (!m_complete || !m_notifier.isEnabled()) {
        return;
    }

    QVector<QByteArray> addDirectories;
    QVector<QByteArray> removeDirectories;

    for (const QString &directory : m_directories) {
        QFileInfo info(directory);
        if (!info.exists()) {
            continue;
        }
        const QByteArray canonicalPath = info.canonicalFilePath().toUtf8();
        if (!addDirectories.contains(canonicalPath)) {
            addDirectories.append(canonicalPath);
        }
    }

    for (auto it = m_watchedDirectories.begin(); it != m_watchedDirectories.end(); ) {
        const int index = addDirectories.indexOf(it->path);

        if (index == -1) {
            inotify_rm_watch(m_inotifyFd, it->id);

            removeDirectories.append(it->path);

            it = m_watchedDirectories.erase(it);
        } else {
            addDirectories.removeAt(index);

            ++it;
        }
    }

    const int watchFlags
            = IN_DONT_FOLLOW
            | IN_DELETE
            | IN_MOVED_FROM
            | IN_DELETE_SELF
            | IN_MOVE_SELF
            | IN_CREATE
            | IN_MOVED_TO;

    for (auto it = addDirectories.begin(); it != addDirectories.end(); ) {
        const QByteArray &directory = *it;

        const int wd = inotify_add_watch(m_inotifyFd, directory.constData(), watchFlags);

        if (wd < 0) {
            it = addDirectories.erase(it);
        } else {
            const WatchedDirectory watch { directory, wd };
            m_watchedDirectories.append(watch);
            ++it;
        }
    }

    if (!addDirectories.isEmpty() || !removeDirectories.isEmpty()) {
        m_scanning = true;
        m_notifier.setEnabled(false);

        const QVector<Capture> originalCaptures = m_captures;

        runAsync([this, originalCaptures, addDirectories, removeDirectories]() {
            scanFiles(originalCaptures, addDirectories, removeDirectories);
        });
    } else if (!m_populated) {
        m_populated = true;

        emit populatedChanged();
    }
}

void CaptureModel::scanFiles(
        const QVector<Capture> &originalCaptures,
        const QVector<QByteArray> &addDirectories,
        const QVector<QByteArray> &removeDirectories)
{
    bool removed = false;

    QVector<Capture> captures = originalCaptures;

    if (!removeDirectories.isEmpty()) {
        auto end = std::remove_if(captures.begin(), captures.end(), [&](const Capture &capture) {
            return removeDirectories.contains(capture.directory);
        });
        if (end != captures.end()) {
            captures.erase(end, captures.end());

            removed = true;
        }
    }

    bool added = false;

    const bool commonFiles = !captures.isEmpty();

    for (const QByteArray &path : addDirectories) {
        DIR *directory = opendir(path.constData());
        if (!directory) {
            continue;
        }

        while (struct dirent64 *entry = readdir64(directory)) {
            if (entry->d_type != DT_REG) {
                continue;
            }

            const QByteArray fileName(entry->d_name);

            if (!isCameraFile(fileName)) {
                continue;
            }

            // do basic validation of file name to exclude things that didn't come from the camera.
            if (fileName.startsWith('.')) {
                continue;
            }

            const Capture capture = { path, fileName, QString() };

            captures.append(capture);
            added = true;
        }

        closedir(directory);
    }

    if (added) {
        std::sort(captures.begin(), captures.end(), compare);
    }

    if (added || removed) {
        diffFiles(captures, originalCaptures, commonFiles);
    } else {
        post([this]() {
            m_notifier.setEnabled(true);

            if (!m_populated) {
                m_populated = true;

                emit populatedChanged();
            }
        });
    }

    QMutexLocker locker(&m_exitMutex);
    m_scanning = false;
    m_exitCondition.wakeOne();
}

void CaptureModel::diffFiles(
        const QVector<Capture> &captures, const QVector<Capture> &expired, const bool commonFiles)
{
    post([this, captures]() {
        m_expiredCaptures = m_captures;
        m_captures = captures;
        m_maximumCaptureIndex = 0;
        m_minimumExpiredIndex = 0;
    });

    int captureBegin = 0;
    int expiredIndex = 0;

    for (int captureIndex = 0;
            commonFiles && captureIndex < captures.count() && expiredIndex < expired.count();
            ++captureIndex) {
        const Capture capture = captures.at(captureIndex);

        int expiredEnd = expiredIndex;

        do {
            if (capture != expired.at(expiredEnd)) {
                ++expiredEnd;
                continue;
            }

            if (captureBegin != captureIndex || expiredEnd != expiredIndex) {
                post([this, captureBegin, captureIndex, expiredIndex, expiredEnd]() {
                    m_maximumCaptureIndex = captureBegin;
                    m_minimumExpiredIndex = expiredIndex;

                    if (expiredEnd > expiredIndex) {
                        beginRemoveRows(
                                    QModelIndex(),
                                    m_maximumCaptureIndex,
                                    m_maximumCaptureIndex + expiredEnd - expiredIndex - 1);
                        m_minimumExpiredIndex = expiredEnd;
                        endRemoveRows();
                    }

                    if (captureIndex > captureBegin) {
                        beginInsertRows(
                                    QModelIndex(),
                                    m_maximumCaptureIndex,
                                    m_maximumCaptureIndex + captureIndex - captureBegin - 1);
                        m_maximumCaptureIndex = captureIndex;
                        endInsertRows();
                    }
                    emit countChanged();
                });
            }

            captureBegin = captureIndex + 1;
            expiredIndex = expiredEnd + 1;
            break;
        } while (expiredEnd < expired.count());
    }

    post([this, captureBegin, expiredIndex]() {
        m_maximumCaptureIndex = captureBegin;
        m_minimumExpiredIndex = expiredIndex;

        bool changed = false;

        if (m_expiredCaptures.count() > expiredIndex) {
            changed = true;

            beginRemoveRows(
                        QModelIndex(),
                        m_maximumCaptureIndex,
                        m_maximumCaptureIndex + m_expiredCaptures.count() - expiredIndex - 1);
            m_minimumExpiredIndex = m_expiredCaptures.count();
            endRemoveRows();
        }

        if (m_captures.count() > captureBegin) {
            changed = true;

            beginInsertRows(
                        QModelIndex(),
                        m_maximumCaptureIndex,
                        m_maximumCaptureIndex + m_captures.count() - captureBegin - 1);
            m_maximumCaptureIndex = m_captures.count();
            endInsertRows();
        }

        m_maximumCaptureIndex = m_captures.count();
        m_minimumExpiredIndex = 0;
        m_expiredCaptures.clear();

        m_notifier.setEnabled(true);

        if (changed) {
            emit countChanged();
        }

        if (!m_populated) {
            m_populated = true;

            emit populatedChanged();
        }
    });
}

void CaptureModel::filesChanged()
{
    bool directoriesChanged = false;

    int bufferSize = 0;
    ioctl(m_inotifyFd, FIONREAD, (char *) &bufferSize);
    QVarLengthArray<char, 4096> buffer(bufferSize);

    bufferSize = read(m_inotifyFd, buffer.data(), bufferSize);
    char *at = buffer.data();
    char * const end = at + bufferSize;

    struct inotify_event *pevent = 0;
    for (;at < end; at += sizeof(inotify_event) + pevent->len) {
        pevent = reinterpret_cast<inotify_event *>(at);

        const WatchedDirectory directory = [&]() -> WatchedDirectory {
            for (const WatchedDirectory &directory : m_watchedDirectories) {
                if (directory.id == pevent->wd) {
                    return directory;
                }
            }
            const WatchedDirectory directory = { QByteArray(), 0 };
            return directory;
        }();

        if (directory.path.isEmpty()) {
            continue;
        }

        if (pevent->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
            directoriesChanged = true;
            continue;
        } else if (pevent->mask & IN_ISDIR) {
            // Ignore directories.
            continue;
        } else if (pevent->len <= 0) {
            // No file name.
            continue;
        }

        if (pevent->mask & (IN_DELETE | IN_MOVED_FROM)) {
            const QByteArray fileName = QByteArray(pevent->name);

            const Capture capture = { directory.path, fileName, QString() };

            auto it = std::lower_bound(m_captures.begin(), m_captures.end(), capture, compare);
            while (it != m_captures.end() && it->fileName == fileName) {
                if (it->directory == directory.path) {
                    int index = std::distance(m_captures.begin(), it);

                    beginRemoveRows(QModelIndex(), index, index);
                    it = m_captures.erase(it);
                    m_maximumCaptureIndex -= 1;
                    endRemoveRows();
                } else {
                    ++it;
                }
            }
        }

        if (pevent->mask & (IN_CREATE | IN_MOVED_TO)) {
            insertCapture(directory, QByteArray(pevent->name), QString());
        }
    }

    if (directoriesChanged) {
        updateWatchedDirectories();
    }
}

void CaptureModel::insertCapture(
        const WatchedDirectory &directory, const QByteArray &fileName, const QString &mimeType)
{
    if (!isCameraFile(fileName)) {
        return;
    }

    const Capture capture = { directory.path, fileName, mimeType };

    const auto insertAt = std::lower_bound(
                m_captures.begin(), m_captures.end(), capture, compare);

    bool duplicate = false;
    for (auto it = insertAt; it != m_captures.end() && it->fileName == fileName; ++it) {
        if (it->directory == directory.path) {
            duplicate = true;
            break;
        }
    }

    if (duplicate) {
        return;
    }

    int index = std::distance(m_captures.begin(), insertAt);

    beginInsertRows(QModelIndex(), index, index);
    m_captures.insert(insertAt, capture);
    m_maximumCaptureIndex += 1;
    endInsertRows();

    emit countChanged();
}

bool CaptureModel::compare(const Capture &left, const Capture &right)
{
    const QByteArray leftName = cameraSortName(left.fileName);
    const QByteArray rightName = cameraSortName(right.fileName);
    return leftName == rightName ? left.fileName > right.fileName : leftName > rightName;
}

bool CaptureModel::isCameraFile(const QByteArray &fileName) const
{
    static const QRegularExpression cameraFileRegEx = [] {
        QRegularExpression regex("\\A(?:ProCam_)?\\d{8}_\\d{6}(?:_\\d+)?.(?:jpg|heic|mp4)\\z");

        regex.optimize();

        return regex;
    }();

    return cameraFileRegEx.match(QString::fromUtf8(fileName)).hasMatch();
}

template <class Function>
void CaptureModel::post(const Function &function)
{
    QCoreApplication::postEvent(this, new FunctionEvent<Function>(function));
}

#include "moc_capturemodel.cpp"
