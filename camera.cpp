// SPDX-FileCopyrightText: 2013 - 2016 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include <QDir>
#include <QTranslator>
#include <QLocale>
#include <QElapsedTimer>
#include <QDebug>

#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlContext>

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
