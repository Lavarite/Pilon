#include <QApplication>
#include "../core/workspace/pilonsettings.h"
#include "../core/workspace/piconfpreferences.h"
#include "../core/workspace/pilonworkspace.h"
#include <QMessageBox>
#include "applicationservice.h"
#include "applicationreset.h"
#include "keybindings.h"
#include "tabletinputguard.h"
#include "../../addons/git/src/gitcredentials.h"
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QDir>
#include <QFile>
#ifdef Q_OS_ANDROID
#include <QStandardPaths>
#include <QJniObject>
#include <QJsonDocument>
#include <QTimer>
#include <QtCore/private/qandroidextras_p.h>
#include "../../addons/calendar/src/calendarmanager.h"
#include "../platform/android/androidstorage.h"
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_ANDROID
    if (argc >= 2 && QByteArray(argv[1]) == "--calendar-reminders") {
        QCoreApplication::setOrganizationName(QStringLiteral("Pilon"));
        QCoreApplication::setApplicationName(QStringLiteral("Pilon"));
        QAndroidService app(argc, argv);
        QTimer::singleShot(0, &app, [] {
            const QJniObject context = QNativeInterface::QAndroidApplication::context();
            const auto generation = QJniObject::callStaticMethod<jlong>("org/pilon/PilonReminderService", "currentGeneration");
            QString error;
            const auto snapshot = generation < 0 ? QVariantMap{} : CalendarManager::reminderSnapshot(
                PilonWorkspace::appDataDirectory() + "/calendar", QDateTime::currentDateTimeUtc(), &error);
            const auto json = QJniObject::fromString(snapshot.isEmpty() ? QString{} : QString::fromUtf8(QJsonDocument::fromVariant(snapshot).toJson(QJsonDocument::Compact)));
            QJniObject::callStaticMethod<void>("org/pilon/PilonReminderService", "complete",
                "(Landroid/content/Context;JLjava/lang/String;Ljava/lang/String;)V", context.object(), generation,
                json.object<jstring>(), QJniObject::fromString(error).object<jstring>());
        });
        return app.exec();
    }
#endif
    if (argc >= 2 && QByteArray(argv[1]) == "--git-credential") {
        QCoreApplication app(argc, argv);
        QFile input, output, errors;
        input.open(stdin, QIODevice::ReadOnly); output.open(stdout, QIODevice::WriteOnly); errors.open(stderr, QIODevice::WriteOnly);
        return GitCredentials::runHelper(argc == 3 ? QString::fromLocal8Bit(argv[2]) : QString(), input, output, errors);
    }
    QCoreApplication::setAttribute(Qt::AA_CompressTabletEvents, false);
    QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);
    // Qt Quick also coalesces touch updates between frames; ink and lassos need every sample.
    qputenv("QML_NO_TOUCH_COMPRESSION", "1");
    QQuickWindow::setDefaultAlphaBuffer(true);

    if (!qEnvironmentVariableIsSet("QT_FFMPEG_DECODING_HW_DEVICE_TYPES")) {
        bool intel = false, nvidia = false;
        for (const auto &card : QDir("/sys/class/drm").entryList({"card*"}, QDir::Dirs)) {
            QFile vendor("/sys/class/drm/" + card + "/device/vendor");
            if (!vendor.open(QIODevice::ReadOnly)) continue;
            const auto id = vendor.readAll().trimmed(); intel |= id == "0x8086"; nvidia |= id == "0x10de";
        }
        if (intel && !nvidia) qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", "vaapi");
    }
    QCoreApplication::setOrganizationName(QStringLiteral("Pilon"));
    QCoreApplication::setApplicationName(QStringLiteral("Pilon"));
    bool background = false;
    for (int i = 1; i < argc; ++i) background |= QByteArray(argv[i]) == "--background";
    if (background && !PilonSettings().value("app/backgroundEnabled", false).toBool()) return 0;
    if (background && qEnvironmentVariable("QT_QPA_PLATFORM") != "offscreen"
        && !ApplicationService::prepareBackgroundDisplay()) return 75;
    QApplication app(argc, argv);
#ifdef Q_OS_ANDROID
    // Android does not provide HOME; helpers need the app's private home directory.
    qputenv("HOME", QFile::encodeName(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).canonicalPath()));
    if (!AndroidStorage::ensureAccess()) return 1;
#endif
    TabletInputGuard tabletInputGuard;
    QGuiApplication::setDesktopFileName(QStringLiteral("pilon"));
    QString resetError;
    bool firstLaunch = true;
    int resumeSetupStep = -1;
    ApplicationService service(background);
    if (!service.claimInstance()) return service.error().isEmpty() ? 0 : 1;
    auto &configuration = PiconfPreferences::instance();
    configuration.reportError = [&service](const QString &error) { service.reportError(error); };
    for (;;) {
        int result;
        QVariantMap keep, ownedPaths;
        QString resetRoot, destination;
        QString storageError;
        while (!PilonWorkspace::initializeStorage(&storageError)) {
            if (QMessageBox::critical(nullptr, QObject::tr("Pilon"), storageError,
                    QMessageBox::Retry | QMessageBox::Cancel, QMessageBox::Retry) != QMessageBox::Retry) return 1;
        }
        const bool previousStartup = PilonSettings().value("app/backgroundEnabled",false).toBool();
        bool configurationReady = configuration.start(PilonWorkspace::configuredRoot(), storageError);
        if (configurationReady && previousStartup != PilonSettings().value("app/backgroundEnabled", false).toBool())
            configurationReady = service.reconcileConfiguredStartup(previousStartup, storageError);
        service.reloadPreferences(background);
        KeyBindings::instance()->reloadPreferences();
        if (!configurationReady) service.reportError(storageError);
        service.setSetupResumeStep(resumeSetupStep); resumeSetupStep = -1;
        if (!resetError.isEmpty()) service.reportError(resetError);
        {
            QQmlApplicationEngine engine;
            engine.setInitialProperties({{"appService", QVariant::fromValue(&service)}, {"startupFiles", firstLaunch ? service.launchFiles() : QStringList{}}});
            firstLaunch = false;
            engine.loadFromModule("Pilon", "Main");
            if (engine.rootObjects().isEmpty()) return -1;
            service.setWindow(qobject_cast<QWindow *>(engine.rootObjects().first()));
            result = app.exec();
            keep = service.resetChoices(); ownedPaths = service.resetOwnedPaths(); resetRoot = service.resetWorkspaceRoot();
            destination = service.workspaceDestination();
            if (result == ApplicationService::WorkspaceExitCode) resumeSetupStep = service.setupResumeStep();
            if (result == ApplicationService::ResetExitCode || result == ApplicationService::WorkspaceExitCode ||
                result == ApplicationService::ConfigurationExitCode) QApplication::setQuitOnLastWindowClosed(false);
        }
        // QML Settings has now persisted its pending writes. Finish this workspace before opening another engine.
        if (result == ApplicationService::ConfigurationExitCode) {
            resetError.clear();
            service.finishConfigurationImport(resetError);
            configuration.stop();
            background = false;
            continue;
        }
        QString syncError;
        const bool saved = configuration.flush(syncError);
        configuration.stop();
        if (!saved && (result == ApplicationService::ResetExitCode || result == ApplicationService::WorkspaceExitCode)) {
            resetError = syncError; background = false; continue;
        }
        if (!saved) qWarning().noquote() << syncError;
        if (result == ApplicationService::WorkspaceExitCode) {
            resetError.clear();
            if (!PilonWorkspace::relocateStorage(destination, &resetError)) resumeSetupStep = -1;
            background = false;
            continue;
        }
        if (result != ApplicationService::ResetExitCode) return result;
        // Destroy QML Settings and feature managers before erasing data/preferences.
        resetError.clear();
        if (ApplicationReset::removeSelected(keep, ownedPaths, {resetRoot}, &resetError)
            && ApplicationReset::clearPreferences(&resetError)) {
            KeyBindings::instance()->setRecording(false);
            KeyBindings::instance()->reset();
        }
        background = false;
    }
}
