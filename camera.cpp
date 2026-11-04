// SPDX-FileCopyrightText: 2013 - 2016 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include <QDir>
#include <QTranslator>
#include <QLocale>
#include <QElapsedTimer>
#include <QDebug>

#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QQmlEngine>
#include <QQmlContext>
#include <QStandardPaths>
#include <QTimer>

static bool cameraStartupLoggingEnabled()
{
    static const bool enabled = !qgetenv("CAMERA_STARTUP_LOG").isEmpty();
    return enabled;
}

#define CAMERA_STARTUP_MARK(scope, timer, format, ...) \
    do { \
        if (cameraStartupLoggingEnabled()) \
            qInfo("CAMERA_STARTUP " scope " %lld ms " format, \
                  static_cast<long long>((timer).elapsed()), ##__VA_ARGS__); \
    } while (false)
#include <QQuickItem>
#include <QQuickView>
#include <QQmlComponent>

#ifdef HAS_BOOSTER
#include <MDeclarativeCache>
#endif

// Starts the Camera2 preview helper with the last run's arguments, paused,
// before the interface loads: opening the camera (about 1.4 s) then runs
// alongside QML instead of after it. Camera2Preview adopts the process when
// its arguments match; unclaimed, it is stopped after 8 s.
static void prelaunchPreviewHelper(QGuiApplication *app)
{
    if (!qEnvironmentVariableIsEmpty("PROCAM_NO_PRELAUNCH"))
        return;
    QFile file(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
               + QStringLiteral("/preview-launch.args"));
    if (!file.open(QIODevice::ReadOnly))
        return;
    QStringList arguments = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    if (arguments.size() < 2 || arguments.at(1) != QLatin1String("--preview"))
        return;
    const QString program = arguments.takeFirst();
    if (!QFileInfo(program).isExecutable())
        return;
    QProcess *helper = new QProcess(app);
    helper->start(program, arguments << QStringLiteral("--start-paused"));
    app->setProperty("procamPreviewPrelaunch", QVariant::fromValue<QObject *>(helper));
    QTimer::singleShot(8000, app, [app, helper]() {
        if (app->property("procamPreviewPrelaunch").value<QObject *>() != helper)
            return;
        app->setProperty("procamPreviewPrelaunch", QVariant());
        helper->kill();
        helper->waitForFinished(1000);
        helper->deleteLater();
    });
}

Q_DECL_EXPORT int main(int argc, char *argv[])
{
    QElapsedTimer startupTimer;
    startupTimer.start();
    CAMERA_STARTUP_MARK("app", startupTimer, "main entered");

    QQuickWindow::setDefaultAlphaBuffer(true);

#ifdef HAS_BOOSTER
    QScopedPointer<QGuiApplication> app(MDeclarativeCache::qApplication(argc, argv));
    QScopedPointer<QQuickView> view(MDeclarativeCache::qQuickView());
#else
    QScopedPointer<QGuiApplication> app(new QGuiApplication(argc, argv));
    QScopedPointer<QQuickView> view(new QQuickView);
#endif
    app->setOrganizationName(QStringLiteral("com.procam"));
    app->setApplicationName(QStringLiteral("procam"));
    prelaunchPreviewHelper(app.data());

    QString path(QLatin1String(DEPLOYMENT_PATH));

    view->engine()->setBaseUrl(QUrl::fromLocalFile(path));
    view->setSource(path + QLatin1String("camera.qml"));
    CAMERA_STARTUP_MARK("app", startupTimer, "camera.qml loaded status=%d", view->status());
    view->setTitle(QStringLiteral("ProCam"));

    if (app->arguments().contains("-desktop")) {
        view->resize(480, 854);
        view->rootObject()->setProperty("_desktop", true);
        view->show();
        CAMERA_STARTUP_MARK("app", startupTimer, "desktop window shown");
    } else {
        view->showFullScreen();
        CAMERA_STARTUP_MARK("app", startupTimer, "fullscreen window shown");
    }
    return app->exec();
}
