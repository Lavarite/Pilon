#include <QApplication>
#include "../core/workspace/pilonsettings.h"
#include "../core/workspace/piconfpreferences.h"
#include "../core/workspace/pilonworkspace.h"
#include <QMessageBox>
#include "applicationservice.h"
#include "applicationreset.h"
#include "keybindings.h"
#include "tabletinputguard.h"
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#ifdef Q_OS_ANDROID
#include <QPushButton>
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
#ifndef Q_OS_ANDROID
    if (argc == 2 && QByteArray(argv[1]) == "--assistant-workspace") {
        QCoreApplication app(argc, argv);
        QCoreApplication::setOrganizationName(QStringLiteral("Pilon"));
        QCoreApplication::setApplicationName(QStringLiteral("Pilon"));
        const auto root = PilonWorkspace::configuredRoot();
        const QFileInfo privateRoot(root + "/.pilon"), pointer(PilonWorkspace::bootstrapFile());
        QString error;
        if (pointer.isSymLink() || privateRoot.isSymLink() || (pointer.exists() && (!privateRoot.isDir() ||
            PilonWorkspace::appDataDirectory() != privateRoot.filePath()))) {
            qWarning().noquote() << "Restore the selected Pilon folder and workspace.ini before setting up Assistant.";
            return 1;
        }
        if (!PilonWorkspace::validateRoot(root, &error)) { qWarning().noquote() << error; return 1; }
        QFile output;
        const auto bytes = QFile::encodeName(PilonWorkspace::assistantAgentDirectory()) + '\n';
        return output.open(stdout, QIODevice::WriteOnly) && output.write(bytes) == bytes.size() ? 0 : 1;
    }
#endif
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
    // Android helpers and Qt temporary files need writable app-private directories.
    qputenv("HOME", QFile::encodeName(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).canonicalPath()));
    qputenv("TMPDIR", QFile::encodeName(QStandardPaths::writableLocation(QStandardPaths::TempLocation)));
#endif
    TabletInputGuard tabletInputGuard;
    QGuiApplication::setDesktopFileName(QStringLiteral("pilon"));
    QString resetError;
    bool firstLaunch = true;
    int resumeSetupStep = -1;
    ApplicationService service(background);
    if (!service.claimInstance()) {
        if (service.error().isEmpty()) return 0;
        qWarning().noquote() << service.error();
        return 1;
    }
    auto &configuration = PiconfPreferences::instance();
    configuration.reportError = [&service](const QString &error) { service.reportError(error); };
    for (;;) {
        int result;
        QVariantMap keep, ownedPaths;
        QString resetRoot, destination;
        QString storageError;
        while (!ApplicationReset::recover(ApplicationReset::locations(), &storageError) || !PilonWorkspace::initializeStorage(&storageError)) {
#ifdef Q_OS_ANDROID
            if (AndroidStorage::needsSharedAccess(PilonWorkspace::configuredRoot())) {
                QMessageBox access(QMessageBox::Warning, QObject::tr("Pilon"),
                    QObject::tr("Allow file access to open your saved Pilon folder."), QMessageBox::Close);
                const auto grant = access.addButton(QObject::tr("Allow file access"), QMessageBox::AcceptRole);
                access.exec();
                if (access.clickedButton() != grant) return 1;
                AndroidStorage::ensureAccess();
                continue;
            }
#endif
            if (QMessageBox::critical(nullptr, QObject::tr("Pilon"), storageError,
                    QMessageBox::Retry | QMessageBox::Cancel, QMessageBox::Retry) != QMessageBox::Retry) return 1;
        }
        const bool configurationReady = service.startConfiguration(storageError);
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
        if (ApplicationReset::reset(keep, ownedPaths, {resetRoot}, &resetError)) {
            KeyBindings::instance()->setRecording(false);
            KeyBindings::instance()->reset();
        }
        background = false;
    }
}
