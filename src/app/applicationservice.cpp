#include "applicationservice.h"
#include "applicationreset.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QUrl>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QFileOpenEvent>
#include <QLocalSocket>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QSaveFile>
#include "../core/workspace/pilonsettings.h"
#include <QStandardPaths>
#include <QTimer>
#include <QNetworkInformation>
#include <QEvent>
#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#else
#include <QFileSystemWatcher>
#include <QInputDevice>
#endif

namespace {
#ifdef Q_OS_ANDROID
QPointer<ApplicationService> androidService;
#else
bool desktopKeyboardAvailable() {
    // X11 can expose a virtual master keyboard even on a touch-only machine.
    const QDir inputDevices(QStringLiteral("/run/udev/data"));
    if (inputDevices.exists()) {
        for (const auto &entry : inputDevices.entryList({QStringLiteral("c13:*")}, QDir::Files)) {
            QFile device(inputDevices.filePath(entry));
            if (device.open(QIODevice::ReadOnly)
                && device.readAll().split('\n').contains("E:ID_INPUT_KEYBOARD=1")) return true;
        }
        return false;
    }
    for (const auto *device : QInputDevice::devices()) {
        if (device->type() == QInputDevice::DeviceType::Keyboard
            && !device->name().contains(QStringLiteral("core keyboard"), Qt::CaseInsensitive)) return true;
    }
    return false;
}
#endif
QString systemdQuoted(QString value) {
    value.replace('\\', "\\\\"); value.replace('"', "\\\"");
    value.replace('%', "%%"); value.replace('$', "$$");
    value.replace('\n', "\\n"); value.replace('\r', "\\r");
    return '"' + value + '"';
}
}

QIcon ApplicationService::columnIcon(const QColor &colour) {
    QIcon icon;
    for (const int size : {16, 22, 32, 64, 128, 256, 512}) {
        QPixmap image(size, size); image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 64., size / 64.);
        painter.setPen(Qt::NoPen); painter.setBrush(colour);
        painter.drawRoundedRect(QRectF(7, 8, 50, 5), 1, 1);
        painter.drawRoundedRect(QRectF(21, 36, 22, 4), 1, 1);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(colour, 3.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(21, 19), QPointF(43, 19));
        QPainterPath scroll;
        scroll.moveTo(23, 21);
        scroll.cubicTo(19, 16, 9, 17, 8, 25);
        scroll.cubicTo(7, 33, 17, 36, 22, 30);
        scroll.cubicTo(26, 25, 22, 20, 17, 21);
        scroll.cubicTo(12, 21, 11, 28, 16, 29);
        scroll.cubicTo(19, 30, 21, 26, 18, 25);
        painter.drawPath(scroll);
        painter.save(); painter.translate(64, 0); painter.scale(-1, 1);
        painter.drawPath(scroll); painter.restore();
        QPainterPath neck;
        neck.moveTo(22, 29); neck.lineTo(25, 33); neck.lineTo(39, 33); neck.lineTo(42, 29);
        painter.drawPath(neck);
        for (const int x : {23, 29, 35, 41}) painter.drawLine(QPointF(x, 42), QPointF(x, 56));
        painter.end(); icon.addPixmap(image);
    }
    return icon;
}

ApplicationService::ApplicationService(bool startHidden, QObject *parent)
    : QObject(parent), m_enabled(PilonSettings().value("app/backgroundEnabled", false).toBool()),
      m_trayEnabled(PilonSettings().value("app/trayEnabled", true).toBool()), m_startHidden(startHidden) {
    PilonSettings settings;
    m_setupRequired = !startHidden && (settings.value("app/setupPending", false).toBool()
        || (!settings.contains("app/setupVersion") && settings.allKeys().isEmpty()));
    if (m_setupRequired) beginSetup();
    qApp->installEventFilter(this);
    QNetworkInformation::loadDefaultBackend();
    if (auto *network = QNetworkInformation::instance()) {
        connect(network, &QNetworkInformation::reachabilityChanged, this, &ApplicationService::connectivityChanged);
        connect(network, &QNetworkInformation::isBehindCaptivePortalChanged, this, &ApplicationService::connectivityChanged);
    }
    connect(m_menu.addAction(tr("Open Pilon")), &QAction::triggered, this, &ApplicationService::showWindow);
    connect(m_menu.addAction(tr("Sync now")), &QAction::triggered, this, &ApplicationService::syncRequested);
    connect(m_menu.addAction(tr("Settings")), &QAction::triggered, this, [this] { showWindow(); emit settingsRequested(); });
    m_menu.addSeparator();
    connect(m_menu.addAction(tr("Quit Pilon")), &QAction::triggered, this, &ApplicationService::quit);
    m_tray.setContextMenu(&m_menu);
    m_tray.setToolTip(tr("Pilon"));
    const auto icon = columnIcon(QApplication::palette().color(QPalette::Highlight));
    m_tray.setIcon(icon);
    QGuiApplication::setWindowIcon(icon);
    connect(&m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) showWindow();
    });
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (auto socket = m_server.nextPendingConnection()) {
            const auto read = [this, socket] {
                if (!socket->canReadLine()) return;
                const auto command = socket->readLine(65537).trimmed();
                if (command.size() > 65536) { socket->disconnectFromServer(); return; }
                if (command == "show") showWindow();
                else if (command == "quit") quit();
                else {
                    QStringList files;
                    const auto entries = QJsonDocument::fromJson(command).object().value("open").toArray();
                    for (const auto &entry : entries) {
                        const QUrl url(entry.toString());
                        const auto suffix = QFileInfo(url.toLocalFile()).suffix().toLower();
                        if (url.isLocalFile() && (suffix == "pin" || suffix == "pical" || suffix == "piext")) files << url.toString();
                        if (files.size() >= 64) break;
                    }
                    if (!files.isEmpty()) {
                        showWindow();
                        if (m_window) emit filesRequested(files); else m_pendingFiles += files;
                    }
                }
                socket->disconnectFromServer();
            };
            connect(socket, &QLocalSocket::readyRead, this, read);
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(3000, socket, [socket] { socket->disconnectFromServer(); });
            read();
        }
    });
#ifdef Q_OS_ANDROID
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationInactive || state == Qt::ApplicationSuspended) emit suspendRequested();
    });
#else
    const auto refreshKeyboard = [this] { setHardwareKeyboardAvailable(desktopKeyboardAvailable()); };
    refreshKeyboard();
    auto *keyboardWatcher = new QFileSystemWatcher({QStringLiteral("/run/udev/data")}, this);
    connect(keyboardWatcher, &QFileSystemWatcher::directoryChanged, this, refreshKeyboard);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [refreshKeyboard](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) refreshKeyboard();
    });
#endif
}

ApplicationService::~ApplicationService() {
#ifdef Q_OS_ANDROID
    if (androidService == this) {
        androidService = nullptr;
        QNativeInterface::QAndroidApplication::runOnAndroidMainThread([] {
            const QJniObject activity = QNativeInterface::QAndroidApplication::context();
            if (activity.isValid()) activity.callMethod<void>("detachNativeObserver");
        });
    }
#endif
}

QRectF ApplicationService::foldBounds() const {
    const auto scale = m_window ? m_window->devicePixelRatio() : 1.;
    return {m_foldBounds.x() / scale, m_foldBounds.y() / scale,
            m_foldBounds.width() / scale, m_foldBounds.height() / scale};
}

QString ApplicationService::foldPosture() const {
    return m_foldPosture == 1 ? QStringLiteral("flat") : m_foldPosture == 2 ? QStringLiteral("half-opened") : QString{};
}

void ApplicationService::setHardwareKeyboardAvailable(bool available) {
    if (m_hardwareKeyboardAvailable == available) return;
    m_hardwareKeyboardAvailable = available;
    emit hardwareKeyboardAvailabilityChanged();
}

void ApplicationService::updateFoldState(const QRect &bounds, bool vertical, bool separating, bool occluding, int posture) {
    const bool valid = posture >= 1 && posture <= 2 && bounds.x() >= 0 && bounds.y() >= 0
        && bounds.width() >= 0 && bounds.height() >= 0;
    const auto next = valid ? bounds : QRect{};
    vertical &= valid; separating &= valid; occluding &= valid;
    if (!valid) posture = 0;
    if (m_foldBounds == next && m_foldVertical == vertical && m_foldSeparating == separating
        && m_foldOccluding == occluding && m_foldPosture == posture) return;
    m_foldBounds = next; m_foldVertical = vertical; m_foldSeparating = separating;
    m_foldOccluding = occluding; m_foldPosture = posture;
    emit foldChanged();
}

bool ApplicationService::eventFilter(QObject *object, QEvent *event) {
    // The Settings window also binds Escape. Let its modal file picker receive
    // the key before Qt Quick considers shortcuts in the transient parent.
    if (event->type() == QEvent::ShortcutOverride
        && static_cast<QKeyEvent *>(event)->matches(QKeySequence::Cancel)
        && qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
        event->accept();
        return true;
    }
    if (object == qApp && event->type() == QEvent::FileOpen
        && receiveWidgetUrl(static_cast<QFileOpenEvent *>(event)->url())) return true;
    if (object == m_window && event->type() == QEvent::DevicePixelRatioChange) emit foldChanged();
    return QObject::eventFilter(object, event);
}

bool ApplicationService::receiveWidgetUrl(const QUrl &url) {
    if (!url.isValid() || url.toEncoded().size() > 2048 || url.scheme() != "pilon"
        || url.authority() != "widget" || url.hasFragment()) return false;
    const auto parts = url.path().split('/');
    static const QRegularExpression owner("^([a-z][a-z0-9-]*\\.)*[a-z][a-z0-9-]*$");
    static const QRegularExpression widget("^[a-z][a-z0-9-]{0,63}$"), action("^[A-Za-z0-9_-]{1,128}$");
    static const QRegularExpression workspace("^[a-f0-9]{64}$"), generation("^[1-9][0-9]{0,15}$");
    if (parts.size() != 3 || !parts[0].isEmpty() || parts[1].size() > 128
        || !owner.match(parts[1]).hasMatch() || !widget.match(parts[2]).hasMatch()) return false;
    const auto query = QUrlQuery(url).queryItems(QUrl::FullyDecoded);
    if (query.size() != 3) return false;
    QMap<QString, QString> fields;
    for (const auto &item : query) { if (fields.contains(item.first)) return false; fields.insert(item.first, item.second); }
    if (fields.keys() != QStringList{"action", "generation", "workspace"}
        || !action.match(fields.value("action")).hasMatch() || !workspace.match(fields.value("workspace")).hasMatch()
        || !generation.match(fields.value("generation")).hasMatch()
        || fields.value("generation").toULongLong() > 9007199254740991ULL) return false;
    const auto value = url.toString(QUrl::FullyEncoded);
    if (m_window) { showWindow(); emit filesRequested({value}); }
    else if (!m_pendingFiles.contains(value)) {
        if (m_pendingFiles.size() >= 8) return false;
        m_pendingFiles.append(value);
    }
    return true;
}

#ifdef Q_OS_ANDROID
void ApplicationService::attachAndroidFoldObserver() {
    static const bool registered = [] {
        QJniEnvironment environment;
        return environment.registerNativeMethods("org/pilon/PilonActivity", {{"nativeFoldChanged", "(IIIIZZZI)V",
            reinterpret_cast<void *>(+[](JNIEnv *, jclass, jint left, jint top, jint width, jint height,
                                        jboolean vertical, jboolean separating, jboolean occluding, jint posture) {
                // Android callbacks arrive on its UI thread; all service state stays on the Qt thread.
                QMetaObject::invokeMethod(qApp, [=] {
                    if (androidService) androidService->updateFoldState({left, top, width, height}, vertical, separating, occluding, posture);
                }, Qt::QueuedConnection);
            })}, {"nativeKeyboardAvailabilityChanged", "(Z)V",
            reinterpret_cast<void *>(+[](JNIEnv *, jclass, jboolean available) {
                QMetaObject::invokeMethod(qApp, [=] {
                    if (androidService) androidService->setHardwareKeyboardAvailable(available);
                }, Qt::QueuedConnection);
            })}, {"nativeWidgetRequested", "(Ljava/lang/String;)V",
            reinterpret_cast<void *>(+[](JNIEnv *, jclass, jstring uri) {
                const auto value = QJniObject(uri).toString();
                if (value.size() > 2048) return;
                QMetaObject::invokeMethod(qApp, [value] {
                    if (androidService) androidService->receiveWidgetUrl(QUrl(value, QUrl::StrictMode));
                }, Qt::QueuedConnection);
            })}});
    }();
    if (!registered) { reportError(tr("Unable to observe the display fold.")); return; }
    androidService = this;
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([] {
        const QJniObject activity = QNativeInterface::QAndroidApplication::context();
        if (activity.isValid()) activity.callMethod<void>("attachNativeObserver");
    });
}
#endif

QUrl ApplicationService::chooseConfigurationBundle(QWindow *owner, bool exporting, const QString &title, const QString &filter)
{
    QFileDialog picker(nullptr, title, workspaceRoot(), filter);
    picker.setAcceptMode(exporting ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
    picker.setFileMode(exporting ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
    if (exporting) picker.setDefaultSuffix(QStringLiteral("piconfpack"));
    picker.setWindowModality(Qt::WindowModal);
    picker.winId();
    picker.windowHandle()->setTransientParent(owner);
    return picker.exec() == QDialog::Accepted ? picker.selectedUrls().value(0) : QUrl{};
}

QString ApplicationService::workspaceRoot() const { return PilonWorkspace::configuredRoot(); }
QString ApplicationService::settingsFile() const { return PilonWorkspace::settingsFile(); }
QUrl ApplicationService::settingsLocation() const { return QUrl::fromLocalFile(settingsFile()); }
QString ApplicationService::modelsDirectory() const { return PilonWorkspace::modelsDirectory(); }
QString ApplicationService::artifactsDirectory() const { return PilonWorkspace::artifactsDirectory(); }
void ApplicationService::reloadPreferences(bool startHidden) {
    m_startHidden = startHidden;
    m_workspaceDestination.clear(); m_preparedResetWorkspaceRoot.clear();
    m_resetChoices.clear(); m_resetOwnedPaths.clear(); m_resetWorkspaceRoot.clear();
    m_error.clear();
    const PilonSettings settings;
    m_enabled = settings.value("app/backgroundEnabled", false).toBool();
    m_trayEnabled = settings.value("app/trayEnabled", true).toBool();
    m_setupRequired = !m_startHidden && (settings.value("app/setupPending", false).toBool()
        || (!settings.contains("app/setupVersion") && settings.allKeys().isEmpty()));
    updateTray();
    emit changed();
}
bool ApplicationService::changeWorkspace(const QString &destination, int resumeStep) {
    m_error.clear();
    if (!PilonWorkspace::validateRoot(destination, &m_error)) { emit changed(); return false; }
    if (QDir::cleanPath(destination) == workspaceRoot()) return true;
    m_quitCancelled = false;
    emit quitRequested();
    if (m_quitCancelled) { showWindow(); return false; }
    m_workspaceDestination = QDir::cleanPath(destination); m_setupResumeStep = resumeStep;
    qApp->exit(WorkspaceExitCode);
    return true;
}

bool ApplicationService::beginSetup() {
    PilonSettings settings;
    settings.setValue("app/setupPending", true);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        m_error = tr("Unable to save setup progress. Check your settings folder permissions.");
        emit changed();
        return false;
    }
    m_setupRequired = true;
    m_error.clear();
    emit changed();
    return true;
}

bool ApplicationService::completeSetup() {
    PilonSettings settings;
    settings.setValue("app/setupVersion", 1);
    settings.remove("app/setupPending");
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        settings.remove("app/setupVersion");
        settings.setValue("app/setupPending", true);
        m_error = tr("Unable to finish setup. Check your settings folder permissions and try again.");
        emit changed();
        return false;
    }
    m_setupRequired = false;
    m_error.clear();
    emit changed();
    return true;
}

bool ApplicationService::prepareReset(const QVariantMap &keep, const QVariantMap &ownedPaths) {
    m_error.clear();
    const auto paths = ApplicationReset::locations();
    if (!m_preparedResetWorkspaceRoot.isEmpty() && paths.workspaceRoot != m_preparedResetWorkspaceRoot) {
        m_preparedResetWorkspaceRoot.clear();
        m_error = tr("The Pilon folder changed. Review the reset choices again.");
        emit changed(); return false;
    }
    if (!ApplicationReset::check(keep, ownedPaths, paths, &m_error)) {
        m_preparedResetWorkspaceRoot.clear(); emit changed(); return false;
    }
    m_preparedResetWorkspaceRoot = paths.workspaceRoot;
    return true;
}

bool ApplicationService::resetToSetup(const QVariantMap &keep, const QVariantMap &ownedPaths) {
    if (!prepareReset(keep, ownedPaths)) return false;
    m_quitCancelled = false;
    m_resetting = true; m_pendingResetChoices = keep; m_pendingResetOwnedPaths = ownedPaths;
    emit quitRequested();
    m_resetting = false; m_pendingResetChoices.clear(); m_pendingResetOwnedPaths.clear();
    if (m_quitCancelled) { showWindow(); return false; }
    if (m_enabled && !setBackgroundEnabled(false)) { m_preparedResetWorkspaceRoot.clear(); return false; }
    m_resetChoices = keep; m_resetOwnedPaths = ownedPaths; m_resetWorkspaceRoot = m_preparedResetWorkspaceRoot;
    qApp->exit(ResetExitCode);
    return true;
}

bool ApplicationService::offline() const {
    const auto *network = QNetworkInformation::instance();
    if (!network) return false;
    const auto reachability = network->reachability();
    return network->isBehindCaptivePortal()
        || reachability == QNetworkInformation::Reachability::Disconnected
        || reachability == QNetworkInformation::Reachability::Local
        || reachability == QNetworkInformation::Reachability::Site;
}

QString ApplicationService::socketName() {
    const auto config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/pilon-"
        + QString::fromLatin1(QCryptographicHash::hash(config.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

bool ApplicationService::claimInstance() {
    const auto contact = [this] {
        QLocalSocket socket;
        socket.connectToServer(socketName());
        if (!socket.waitForConnected(300)) return false;
        const auto files = launchFiles();
        socket.write(files.isEmpty() ? QByteArray(m_startHidden ? "background\n" : "show\n")
            : QJsonDocument(QJsonObject{{"open", QJsonArray::fromStringList(files)}}).toJson(QJsonDocument::Compact) + '\n');
        socket.waitForBytesWritten(1000);
        return true;
    };
    if (contact()) return false;
    m_lock = std::make_unique<QLockFile>(socketName() + ".lock");
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(2000)) {
        if (!contact()) m_error = tr("Another Pilon instance is starting. Try opening it again.");
        return false;
    }
    QLocalServer::removeServer(socketName());
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server.listen(socketName())) { m_error = m_server.errorString(); return false; }
    updateTray();
    return true;
}

QStringList ApplicationService::launchFiles() const {
    QStringList files;
    for (const auto &argument : QCoreApplication::arguments().mid(1)) {
        const QUrl url = argument.startsWith("file:") ? QUrl(argument) : QUrl::fromLocalFile(QFileInfo(argument).absoluteFilePath());
        const auto suffix = QFileInfo(url.toLocalFile()).suffix().toLower();
        if (url.isLocalFile() && (suffix == "pin" || suffix == "pical" || suffix == "piext")) files << url.toString();
    }
    return files;
}

void ApplicationService::setWindow(QWindow *window) {
    if (m_window) m_window->removeEventFilter(this);
    m_window = window;
    if (m_window) m_window->setIcon(m_tray.icon());
    emit foldChanged();
#ifdef Q_OS_ANDROID
    if (m_window) {
        // Qt resizes maximized native windows even while setup keeps the workspace hidden.
        m_window->setWindowState(Qt::WindowMaximized);
        m_window->create();
        attachAndroidFoldObserver();
    }
#endif
    if (m_window && !m_pendingFiles.isEmpty()) { emit filesRequested(m_pendingFiles); m_pendingFiles.clear(); }
    if (m_showPending || (m_startHidden && !m_trayEnabled)) showWindow();
}
bool ApplicationService::setIconColours(const QColor &foreground, const QColor &background, const QColor &accent) {
    if (!foreground.isValid() || !background.isValid() || !accent.isValid()) return false;
    const auto ink = QColor::fromRgb(foreground.rgb()), surface = QColor::fromRgb(background.rgb());
    if (m_iconPublished && m_iconForeground == ink && m_iconBackground == surface && m_notificationAccent == accent) return true;
    m_iconForeground = ink; m_iconBackground = surface; m_notificationAccent = accent; m_iconPublished = false;
    const auto icon = columnIcon(ink);
    m_tray.setIcon(icon);
    QGuiApplication::setWindowIcon(icon);
    for (auto *window : QGuiApplication::topLevelWindows()) window->setIcon(icon);
#ifdef Q_OS_ANDROID
    const auto folder = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const auto path = folder + "/pilon-icon.png";
    if (!QDir().mkpath(folder) || QFileInfo(path).isSymLink()) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !icon.pixmap(256, 256).save(&file, "PNG") || !file.commit()) return false;
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([path, surface, accent] {
        const QJniObject activity = QNativeInterface::QAndroidApplication::context();
        if (QNativeInterface::QAndroidApplication::isActivityContext() && activity.isValid())
            activity.callMethod<void>("applyIcon", "(Ljava/lang/String;II)V",
            QJniObject::fromString(path).object<jstring>(), jint(surface.rgb()), jint(accent.rgb()));
    });
#else
    const auto folder = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + "/icons/hicolor/scalable/apps";
    const auto path = folder + "/pilon.svg";
    QFile artwork(":/qt/qml/Pilon/icons/pilon.svg");
    if (!artwork.open(QIODevice::ReadOnly) || !QDir().mkpath(folder) || QFileInfo(path).isSymLink()) return false;
    const auto bytes = artwork.readAll().replace("currentColor", ink.name(QColor::HexRgb).toUtf8());
    QFile previous(path);
    if (!previous.open(QIODevice::ReadOnly) || previous.readAll() != bytes) {
        previous.close();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return false;
        const auto cache = QStandardPaths::findExecutable("gtk-update-icon-cache");
        if (!cache.isEmpty()) QProcess::startDetached(cache, {"-f", "-t", folder + "/../.."});
    }
#endif
    m_iconPublished = true;
    return true;
}
void ApplicationService::showWindow() {
    if (!m_window) { m_showPending = true; return; }
    m_showPending = false;
#ifdef Q_OS_ANDROID
    m_window->show();
#else
    if (m_window->windowState() == Qt::WindowMinimized) m_window->showNormal();
    else m_window->show();
#endif
    m_window->raise(); m_window->requestActivate();
}
void ApplicationService::hideWindow() {
    if (!m_trayEnabled) return;
    for (auto *window : QGuiApplication::topLevelWindows()) window->hide();
}
void ApplicationService::quit() {
    m_quitCancelled = false;
    emit quitRequested();
    if (m_quitCancelled) { showWindow(); return; }
    qApp->quit();
}
void ApplicationService::updateTray() {
    QApplication::setQuitOnLastWindowClosed(!m_trayEnabled);
    if (QGuiApplication::platformName() != "offscreen" && QGuiApplication::platformName() != "minimal")
        m_tray.setVisible(m_trayEnabled);
}

QString ApplicationService::unitContents(const QString &executable) {
    return "[Unit]\nDescription=Pilon background sync and reminders\nStartLimitIntervalSec=0\n\n"
           "[Service]\nType=simple\nExecStart=" + systemdQuoted(executable)
        + " --background\nRestart=on-failure\nRestartSec=5\nTimeoutStopSec=15\n\n[Install]\nWantedBy=default.target\n";
}
bool ApplicationService::configureStartup(bool enabled) {
    const auto folder = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/systemd/user";
    const auto path = folder + "/pilon.service";
    if (!enabled && !QFileInfo::exists(path)
        && !QFileInfo(folder + "/default.target.wants/pilon.service").isSymLink()) return true;
    const auto systemctl = QStandardPaths::findExecutable("systemctl");
    if (systemctl.isEmpty()) { m_error = tr("systemctl is required to manage background startup."); return false; }
    QFile old(path); const bool existed = old.exists();
    QByteArray previous;
    if (existed) {
        if (!old.open(QIODevice::ReadOnly)) { m_error = old.errorString(); return false; }
        previous = old.readAll(); old.close();
    }
    const auto write = [&](const QByteArray &bytes) {
        if (!QDir().mkpath(folder)) return false;
        QSaveFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    };
    if (enabled && !write(unitContents(QCoreApplication::applicationFilePath()).toUtf8())) {
        m_error = tr("Unable to save the background service."); return false;
    }
    const auto run = [&](const QStringList &arguments) {
        QProcess process; process.start(systemctl, QStringList{"--user"} + arguments);
        if (!process.waitForFinished(5000)) { process.kill(); process.waitForFinished(); return false; }
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    };
    if (!run({"daemon-reload"}) || !run({enabled ? "enable" : "disable", "pilon.service"})) {
        if (enabled) { if (existed) write(previous); else QFile::remove(path); run({"daemon-reload"}); }
        m_error = tr("Unable to change background startup. Check that your user systemd service is available.");
        return false;
    }
    if (!enabled && QFile::exists(path)) { QFile::remove(path); run({"daemon-reload"}); }
    return true;
}
bool ApplicationService::setBackgroundEnabled(bool enabled) {
    if (enabled == m_enabled) return true;
    m_error.clear();
    if (!configureStartup(enabled)) { emit changed(); return false; }
    m_enabled = enabled;
    PilonSettings().setValue("app/backgroundEnabled", enabled);
    emit changed();
    return true;
}
bool ApplicationService::setTrayEnabled(bool enabled) {
    if (enabled == m_trayEnabled) return true;
    PilonSettings settings; const auto previous = settings.value("app/trayEnabled");
    settings.setValue("app/trayEnabled", enabled); settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (previous.isValid()) settings.setValue("app/trayEnabled", previous); else settings.remove("app/trayEnabled");
        m_error = tr("Unable to save the tray setting. Check your settings folder permissions and try again.");
        emit changed(); return false;
    }
    m_trayEnabled = enabled; m_error.clear(); updateTray();
    if (!enabled && m_window && !m_window->isVisible()) showWindow();
    emit changed();
    return true;
}

bool ApplicationService::prepareBackgroundDisplay() {
    // On Hyprland the user manager can start before the compositor and may not
    // inherit its display environment. Retry through systemd until a live socket exists.
    const auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    auto names = QDir(runtime).entryList({"wayland-*"}, QDir::System | QDir::NoDotAndDotDot);
    const auto configured = qEnvironmentVariable("WAYLAND_DISPLAY");
    if (!configured.isEmpty()) names.prepend(configured);
    for (const auto &name : names) {
        if (name.endsWith(".lock")) continue;
        QLocalSocket socket;
        socket.connectToServer(QDir::isAbsolutePath(name) ? name : runtime + '/' + name);
        if (!socket.waitForConnected(100)) continue;
        qputenv("WAYLAND_DISPLAY", name.toUtf8());
        qputenv("QT_QPA_PLATFORM", "wayland");
        return true;
    }
    return !qEnvironmentVariableIsEmpty("DISPLAY");
}
