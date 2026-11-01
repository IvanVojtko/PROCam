// SPDX-FileCopyrightText: 2014 - 2021 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include <QQmlExtensionPlugin>
#include <QTranslator>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QLocale>

#include <qqml.h>

#include "capturemodel.h"
#include "camera2preview.h"
#include "declarativecameraextensions.h"
#include "declarativesettings.h"
#include "cameraconfigs.h"

template <typename T> static QObject *singletonFactory(QQmlEngine *, QJSEngine *)
{
    return new T;
}

// using custom translator so it gets properly removed from qApp when engine is deleted
class AppTranslator: public QTranslator
{
public:
    AppTranslator(QObject *parent)
        : QTranslator(parent)
    {
        qApp->installTranslator(this);
    }

    virtual ~AppTranslator()
    {
        qApp->removeTranslator(this);
    }
};


class CameraPlugin : public QQmlExtensionPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.vivid.camera")

public:

    void initializeEngine(QQmlEngine *engine, const char *uri)
    {
        Q_UNUSED(uri)
        Q_UNUSED(engine)
        Q_ASSERT(QLatin1String(uri) == QLatin1String("com.vivid.camera"));

        AppTranslator *engineeringEnglish = new AppTranslator(engine);
        AppTranslator *translator = new AppTranslator(engine);
        engineeringEnglish->load("procam_eng_en", "/usr/share/translations");
        translator->load(QLocale(), "procam", "-", "/usr/share/translations");
    }

    virtual void registerTypes(const char *uri)
    {
        Q_UNUSED(uri)
        Q_ASSERT(QLatin1String(uri) == QLatin1String("com.vivid.camera"));

        qmlRegisterType<CaptureModel>("com.vivid.camera", 1, 0, "CaptureModel");
        qmlRegisterType<Camera2Preview>("com.vivid.camera", 1, 0, "Camera2Preview");
        qmlRegisterType<DeclarativeCameraExtensions>("com.vivid.camera", 1, 0, "CameraExtensions");
        qmlRegisterType<DeclarativeSettings>("com.vivid.camera", 1, 0, "SettingsBase");
        qmlRegisterSingletonType<DeclarativeSettings>("com.vivid.camera", 1, 0, "Settings", DeclarativeSettings::factory);
        qmlRegisterSingletonType<CameraConfigs>("com.vivid.camera", 1, 0, "CameraConfigs", singletonFactory<CameraConfigs>);
    }
};

#include "cameraplugin.moc"
