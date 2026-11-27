#include "tailscalemanager.h"
#include "../../../src/core/network/networksnapshot.h"

#include <QDesktopServices>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QHostAddress>
#include <QSet>
#include <QUuid>
#include "../../../src/core/workspace/pilonsettings.h"
#include <QStandardPaths>
#include <QUrl>
#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#endif

#include <algorithm>
#include <utility>

namespace {
constexpr auto includedDevicesKey = "tailscale/includedDevices";
constexpr auto deviceSelectionInitializedKey = "tailscale/deviceSelectionInitialized";
constexpr auto savedDevicesKey = "ssh/savedDevices";
constexpr auto legacySavedDevicesKey = "tailscale/savedDevices";

QString savedAddress(const QString &input)
{
    QString host = input.trimmed();
    if (host.startsWith('[') && host.endsWith(']')) host = host.mid(1, host.size() - 2);
    QHostAddress ip;
    if (ip.setAddress(host)) return ip.toString();
    if (host.contains(':') || QRegularExpression(QStringLiteral("^[0-9.]+\\.[0-9.]+$")).match(host).hasMatch()) return {};
    const auto ascii = QString::fromLatin1(QUrl::toAce(host));
    static const QRegularExpression dns(QStringLiteral(R"(^(?=.{1,253}$)[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*\.?$)"));
    if (!dns.match(ascii).hasMatch()) return {};
    auto normalized = ascii.toLower();
    if (normalized.endsWith('.')) normalized.chop(1);
    return normalized;
}

QString deviceAddress(const QJsonObject &device)
{
    const QJsonArray addresses = device.value(QStringLiteral("TailscaleIPs")).toArray();
    for (const QJsonValue &address : addresses) {
        const QString value = address.toString();
        if (value.contains(QLatin1Char('.')))
            return value;
    }

    return addresses.isEmpty() ? QString{} : addresses.first().toString();
}

QVariantMap deviceDetails(const QJsonObject &device, const QString &fallbackId, bool self)
{
    QString id = device.value(QStringLiteral("ID")).toString();
    if (id.isEmpty())
        id = fallbackId;

    const QString sshAlias = device.value(QStringLiteral("DNSName")).toString().section(QLatin1Char('.'), 0, 0);
    QString name = sshAlias;
    if (name.isEmpty())
        name = device.value(QStringLiteral("HostName")).toString();
    if (name.isEmpty())
        name = id;

    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("ip"), deviceAddress(device)},
        {QStringLiteral("sshAlias"), sshAlias},
        {QStringLiteral("online"), self || device.value(QStringLiteral("Online")).toBool()},
        {QStringLiteral("onlineKnown"), true},
        {QStringLiteral("self"), self},
    };
}
}

TailscaleManager::TailscaleManager(QObject *parent)
    : QObject(parent)
    , m_tailscaleExecutable(QStandardPaths::findExecutable(QStringLiteral("tailscale")))
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (!cancelDiscoveryRequest()) return;
#ifdef Q_OS_ANDROID
        cancelAndroid();
#endif
        m_action = Action::None; m_cleanupPending=false;
        if (m_statusProcess.state() != QProcess::NotRunning) m_statusProcess.kill();
        if (m_actionProcess.state() != QProcess::NotRunning) m_actionProcess.kill();
        setStatusKnown(false);
        finishDiscovery(tr("Tailscale did not respond. Check the service and try again."));
    });
#ifdef Q_OS_ANDROID
    m_androidPoll.setInterval(100);
    connect(&m_androidPoll, &QTimer::timeout, this, &TailscaleManager::pollAndroid);
    QTimer::singleShot(0, this, &TailscaleManager::restoreDiscovery);
#endif
    connect(&m_statusProcess, &QProcess::finished,
            this, &TailscaleManager::handleStatusFinished);
    connect(&m_statusProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;

        m_timeout.stop(); m_cleanupPending=false;
        setStatusKnown(false);
        setServiceRunning(false);
        setLoggedIn(false);
        setVpnDisconnected(true);
        showSavedPeers();
        setErrorMessage(tr("Unable to run Tailscale."));
        setBusy(false);
    });

    m_actionProcess.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_actionProcess, &QProcess::readyReadStandardOutput,
            this, &TailscaleManager::readActionOutput);
    connect(&m_actionProcess, &QProcess::finished,
            this, &TailscaleManager::handleActionFinished);
    connect(&m_actionProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;

        m_timeout.stop();
        m_action = Action::None;
        setErrorMessage(tr("Unable to start the requested Tailscale action."));
        setBusy(false);
    });
}

TailscaleManager::~TailscaleManager() {
    cancelDiscoveryRequest();
#ifdef Q_OS_ANDROID
    cancelAndroid();
#endif
    m_timeout.stop();
    QObject::disconnect(&m_timeout, nullptr, this, nullptr);
    for (auto *process : {&m_statusProcess, &m_actionProcess})
        QObject::disconnect(process, nullptr, this, nullptr);
    for (auto *process : {&m_statusProcess, &m_actionProcess}) {
        if (process->state() == QProcess::NotRunning) continue;
        process->kill(); process->waitForFinished(1000);
    }
}

bool TailscaleManager::serviceRunning() const
{
    return m_serviceRunning;
}

bool TailscaleManager::loggedIn() const
{
    return m_loggedIn;
}

bool TailscaleManager::busy() const
{
    return m_busy;
}

QString TailscaleManager::errorMessage() const
{
    return m_errorMessage;
}

QVariantList TailscaleManager::devices() const
{
    return m_devices;
}

QVariantList TailscaleManager::includedDevices() const
{
    return m_includedDevices;
}

bool TailscaleManager::externalApplication() const
{
#ifdef Q_OS_ANDROID
    return true;
#else
    return false;
#endif
}

bool TailscaleManager::externalAppInstalled() const
{
#ifdef Q_OS_ANDROID
    return m_externalAppInstalled;
#else
    return !m_tailscaleExecutable.isEmpty();
#endif
}

QVariantList TailscaleManager::loadSavedDevices(QString *error) const
{
    const PilonSettings settings;
    const auto bytes = settings.value(savedDevicesKey, settings.value(legacySavedDevicesKey)).toByteArray();
    if (bytes.isEmpty()) return {};
    QJsonParseError parse{};
    const auto document = bytes.size() <= 1024 * 1024 ? QJsonDocument::fromJson(bytes, &parse) : QJsonDocument{};
    QVariantList result;
    QSet<QString> ids, addresses;
    static const QRegularExpression id(QStringLiteral("^external:[a-f0-9]{32}$"));
    if (parse.error != QJsonParseError::NoError || !document.isArray() || document.array().size() > 1024) {
        if (error) *error = tr("Saved devices could not be read. Restore the SSH settings before editing them.");
        return {};
    }
    for (const auto &entry : document.array()) {
        auto device = entry.toObject().toVariantMap();
        const auto name = device.value("name").toString(), address = device.value("ip").toString(), identity = device.value("id").toString();
        if (!entry.isObject() || entry.toObject().size() != 4 || !id.match(identity).hasMatch() || ids.contains(identity)
            || name.isEmpty() || name.size() > 128 || name != name.trimmed()
            || name.contains(QRegularExpression(QStringLiteral("[\\x{0000}-\\x{001f}\\x{007f}]")))
            || address.isEmpty() || savedAddress(address) != address || addresses.contains(address)
            || !entry.toObject().value("included").isBool()) {
            if (error) *error = tr("Saved devices could not be read. Restore the SSH settings before editing them.");
            return {};
        }
        ids.insert(identity); addresses.insert(address);
        device.insert("online", false); device.insert("onlineKnown", false);
        device.insert("self", false); device.insert("saved", true);
        result.append(device);
    }
    return result;
}

QVariantList TailscaleManager::savedDevices() const { return loadSavedDevices(); }

void TailscaleManager::openExternalApplication()
{
#ifdef Q_OS_ANDROID
    externalAction("open");
#endif
}

void TailscaleManager::disconnectExternalApplication()
{
#ifdef Q_OS_ANDROID
    externalAction("disconnect");
#endif
}

bool TailscaleManager::persistSavedDevices(const QVariantList &devices)
{
    QJsonArray stored;
    for (const auto &entry : devices) {
        const auto device = entry.toMap();
        stored.append(QJsonObject{{"id", device.value("id").toString()}, {"name", device.value("name").toString()},
            {"ip", device.value("ip").toString()}, {"included", device.value("included").toBool()}});
    }
    PilonSettings settings;
    const bool existed = settings.contains(savedDevicesKey);
    const auto previous = settings.value(savedDevicesKey);
    settings.setValue(savedDevicesKey, QJsonDocument(stored).toJson(QJsonDocument::Compact)); settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (existed) settings.setValue(savedDevicesKey, previous); else settings.remove(savedDevicesKey);
        settings.sync(); setErrorMessage(tr("Unable to save devices. Check the Pilon folder permissions.")); return false;
    }
    setErrorMessage({}); emit savedDevicesChanged();
    if (externalApplication()) mergeExternalDevices();
    return true;
}

bool TailscaleManager::saveDevice(const QString &id, const QString &name, const QString &address)
{
    if (m_busy) { setErrorMessage(tr("Wait for the device operation to finish.")); return false; }
    const auto trimmedName = name.trimmed(), host = savedAddress(address);
    if (trimmedName.isEmpty() || trimmedName.size() > 128
        || trimmedName.contains(QRegularExpression(QStringLiteral("[\\x{0000}-\\x{001f}\\x{007f}]")))
        || host.isEmpty()) { setErrorMessage(tr("Enter a name and a valid IP address or DNS name.")); return false; }
    QString error;
    auto devices = loadSavedDevices(&error);
    if (!error.isEmpty()) { setErrorMessage(error); return false; }
    qsizetype found = -1;
    for (qsizetype index = 0; index < devices.size(); ++index) {
        const auto device = devices[index].toMap();
        if (device.value("id").toString() == id) found = index;
        else if (device.value("ip").toString() == host) { setErrorMessage(tr("This address is already saved.")); return false; }
    }
    if ((!id.isEmpty() && found < 0) || (id.isEmpty() && devices.size() >= 1024)) {
        setErrorMessage(id.isEmpty() ? tr("Too many saved devices.") : tr("This device is no longer saved.")); return false;
    }
    QVariantMap device{{"id", id.isEmpty() ? "external:" + QUuid::createUuid().toString(QUuid::Id128) : id},
        {"name", trimmedName}, {"ip", host}, {"included", found < 0 || devices[found].toMap().value("included").toBool()}};
    if (found < 0) devices.append(device); else devices[found] = device;
    return persistSavedDevices(devices);
}

bool TailscaleManager::removeDevice(const QString &id)
{
    if (m_busy) { setErrorMessage(tr("Wait for the device operation to finish.")); return false; }
    QString error; auto devices = loadSavedDevices(&error);
    if (!error.isEmpty()) { setErrorMessage(error); return false; }
    const auto before = devices.size();
    devices.removeIf([&](const QVariant &entry) { return entry.toMap().value("id").toString() == id; });
    if (devices.size() == before) { setErrorMessage(tr("This device is no longer saved.")); return false; }
    return persistSavedDevices(devices);
}

bool TailscaleManager::usedByPilon() const
{
    const PilonSettings settings;
    const auto included = settings.value(QLatin1String(includedDevicesKey)).toStringList();
    return settings.value("tailscale/usedByPilon", false).toBool()
        || !settings.value("tailscale/usedIdentity").toString().isEmpty()
        || std::any_of(included.cbegin(), included.cend(), [](const auto &id) { return !id.startsWith("api:"); });
}

bool TailscaleManager::accountClaimed() const
{
    return m_statusKnown && m_loggedIn && !m_currentIdentity.isEmpty()
        && PilonSettings().value("tailscale/usedIdentity").toString() == m_currentIdentity;
}

void TailscaleManager::setStatusKnown(bool known)
{
    if (m_statusKnown == known) return;
    m_statusKnown = known; emit statusKnownChanged(); emit usedByPilonChanged();
}

bool TailscaleManager::useCurrentAccount()
{
    if (!m_statusKnown || !m_loggedIn || m_currentIdentity.isEmpty()) {
        setErrorMessage(tr("Refresh the signed-in account before using it in Pilon.")); return false;
    }
    return persistUseReceipt(m_currentIdentity);
}

bool TailscaleManager::persistUseReceipt(const QString &identity)
{
    PilonSettings settings;
    const QStringList keys{"tailscale/usedIdentity", "tailscale/usedByPilon"};
    QVariantMap before;
    for (const auto &key : keys) if (settings.contains(key)) before[key]=settings.value(key);
    if (!identity.isEmpty()) settings.setValue(keys[0],identity);
    settings.setValue(keys[1],true);settings.sync();
    if (settings.status()!=QSettings::NoError) {
        for (const auto &key : keys) {
            if (before.contains(key)) settings.setValue(key,before.value(key));
            else settings.remove(key);
        }
        settings.sync();
        setErrorMessage(tr("Unable to save the device account. Check the Pilon folder permissions."));return false;
    }
    setErrorMessage({});emit usedByPilonChanged();return true;
}

QString TailscaleManager::cleanupBlockedReason() const
{
    if (m_busy) return tr("Wait for the device account operation to finish.");
    if (!usedByPilon()) return {};
    if (!m_statusKnown) return m_errorMessage.isEmpty() ? tr("Refresh the device account before continuing.") : m_errorMessage;
    if (m_loggedIn && PilonSettings().value("tailscale/usedIdentity").toString().isEmpty())
        return tr("Open Devices settings and choose Use in Pilon, or sign out, before continuing.");
    return {};
}

bool TailscaleManager::forgetUseReceipt()
{
    PilonSettings settings;
    settings.remove("tailscale/usedIdentity"); settings.remove("tailscale/usedByPilon");
    settings.remove(QLatin1String(includedDevicesKey)); settings.remove(QLatin1String(deviceSelectionInitializedKey)); settings.sync();
    if (settings.status() != QSettings::NoError) {
        setErrorMessage(tr("Unable to clear the device account settings. Check the Pilon folder permissions.")); return false;
    }
    NetworkSnapshot::clear("tailscale-status");
    emit usedByPilonChanged(); return true;
}

void TailscaleManager::logoutUsedAccount()
{
    const auto blocked=cleanupBlockedReason();
    if (!blocked.isEmpty()) {setErrorMessage(blocked);return;}
    if (!usedByPilon()) return;
    m_cleanupPending=true;
#ifdef Q_OS_ANDROID
    // This cleanup is for Pilon's old private node, never the external app's account.
    startAndroidAction(Action::None);
#else
    refresh();
#endif
}

void TailscaleManager::finishUsedAccountCleanup()
{
    m_cleanupPending=false;
#ifdef Q_OS_ANDROID
    if (!m_loggedIn) { forgetUseReceipt(); refresh(); return; }
    startAndroidAction(Action::Logout);
#else
    const auto identity=PilonSettings().value("tailscale/usedIdentity").toString();
    if (!m_loggedIn || identity != m_currentIdentity) {forgetUseReceipt();return;}
    logout();
#endif
}

void TailscaleManager::refresh(bool clearError)
{
#ifdef Q_OS_ANDROID
    if (m_busy) return;
    if (clearError) setErrorMessage({});
    refreshExternalApplication();
    if (m_discoveryConnected && (clearError || QDateTime::currentMSecsSinceEpoch() >= m_discoveryRefreshAt)) refreshDiscovery();
#else
    Q_UNUSED(clearError);
    if (m_statusProcess.state() != QProcess::NotRunning || m_actionProcess.state() != QProcess::NotRunning)
        return;

    if (m_devices.isEmpty()) showSavedPeers();
    const auto executable = QStandardPaths::findExecutable(QStringLiteral("tailscale"));
    if (executable != m_tailscaleExecutable) {
        m_tailscaleExecutable = executable;
        emit externalStatusChanged();
    }
    if (m_tailscaleExecutable.isEmpty()) {
        setStatusKnown(false); setServiceRunning(false); setLoggedIn(false); showSavedPeers();
        setVpnDisconnected(true);
        setErrorMessage(tr("Tailscale is not installed."));
        return;
    }

    setBusy(true);
    setStatusKnown(false);
    setErrorMessage({});
    m_timeout.start(15000);
    m_statusProcess.start(m_tailscaleExecutable, {QStringLiteral("status"), QStringLiteral("--json")});
#endif
}

void TailscaleManager::startService()
{
#ifdef Q_OS_ANDROID
    externalAction("connect");
#else
    const QString systemctl = QStandardPaths::findExecutable(QStringLiteral("systemctl"));
    if (systemctl.isEmpty()) {
        setErrorMessage(tr("Starting Tailscale requires systemctl on this system."));
        return;
    }

    startPrivilegedAction(systemctl,
                          {QStringLiteral("start"), QStringLiteral("tailscaled.service")},
                          Action::StartService);
#endif
}

void TailscaleManager::login()
{
#ifdef Q_OS_ANDROID
    openExternalApplication();
    return;
#endif
    if (!m_serviceRunning) {
        setErrorMessage(tr("Start the Tailscale service before logging in."));
        return;
    }

    if (m_busy) {setErrorMessage(tr("Wait for the device account operation to finish."));return;}
    if (!persistUseReceipt()) return;
#ifdef Q_OS_ANDROID
    startAndroidAction(Action::Login);
#else
    startPrivilegedAction(m_tailscaleExecutable,
                          {QStringLiteral("login"), QStringLiteral("--qr=false"),
                           QStringLiteral("--timeout=5m")},
                          Action::Login);
#endif
}

void TailscaleManager::logout()
{
#ifdef Q_OS_ANDROID
    openExternalApplication();
    return;
#endif
    if (!m_loggedIn)
        return;

#ifdef Q_OS_ANDROID
    startAndroidAction(Action::Logout);
#else
    startPrivilegedAction(m_tailscaleExecutable,
                          {QStringLiteral("logout")},
                          Action::Logout);
#endif
}

void TailscaleManager::setDeviceIncluded(const QString &deviceId, bool included)
{
    if (deviceId.isEmpty())
        return;
    const bool discovered = m_discoveryConnected && deviceId.startsWith("api:");
    if (deviceId.startsWith("external:") || (externalApplication() && !discovered)) {
        QString error; auto devices = loadSavedDevices(&error);
        if (!error.isEmpty()) { setErrorMessage(error); return; }
        for (auto &entry : devices) {
            auto device = entry.toMap();
            if (device.value("id").toString() != deviceId) continue;
            device.insert("included", included); entry = device;
            persistSavedDevices(devices); return;
        }
        setErrorMessage(tr("This device is no longer saved.")); return;
    }
    if ((externalApplication() || discovered) && std::none_of(m_devices.cbegin(), m_devices.cend(), [&](const auto &entry) { return entry.toMap().value("id").toString() == deviceId; })) {
        setErrorMessage(tr("This device is no longer available.")); return;
    }
    if (included && !externalApplication() && !discovered && !useCurrentAccount()) return;

    PilonSettings settings;
    QStringList includedIds = settings.value(QLatin1String(includedDevicesKey)).toStringList();
    if (included && !includedIds.contains(deviceId))
        includedIds.append(deviceId);
    else if (!included)
        includedIds.removeAll(deviceId);

    settings.setValue(QLatin1String(includedDevicesKey), includedIds);
    settings.setValue(QLatin1String(deviceSelectionInitializedKey), true);
    settings.sync();
    if (settings.status()!=QSettings::NoError) {
        setErrorMessage(tr("Unable to save included devices. Check the Pilon folder permissions."));return;
    }

    bool changed = false;
    for (QVariant &deviceValue : m_devices) {
        QVariantMap device = deviceValue.toMap();
        if (device.value(QStringLiteral("id")).toString() != deviceId)
            continue;

        if (device.value(QStringLiteral("included")).toBool() != included) {
            device.insert(QStringLiteral("included"), included);
            deviceValue = device;
            changed = true;
        }
        break;
    }

    if (changed)
        emit devicesChanged();
    rebuildIncludedDevices();
}

void TailscaleManager::setServiceRunning(bool running)
{
    if (m_serviceRunning == running)
        return;
    m_serviceRunning = running;
    emit serviceRunningChanged();
}

void TailscaleManager::setVpnDisconnected(bool disconnected)
{
    if (m_vpnDisconnected == disconnected) return;
    m_vpnDisconnected = disconnected;
    emit vpnDisconnectedChanged();
}

void TailscaleManager::setLoggedIn(bool loggedIn)
{
    if (m_loggedIn == loggedIn)
        return;
    m_loggedIn = loggedIn;
    emit loggedInChanged();
}

void TailscaleManager::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void TailscaleManager::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message)
        return;
    m_errorMessage = message;
    emit errorMessageChanged();
}

void TailscaleManager::setDevices(QVariantList devices)
{
    if (m_devices == devices)
        return;
    m_devices = std::move(devices);
    emit devicesChanged();
    rebuildIncludedDevices();
}

void TailscaleManager::rebuildIncludedDevices()
{
    QVariantList includedDevices;
    for (const QVariant &deviceValue : std::as_const(m_devices)) {
        const QVariantMap device = deviceValue.toMap();
        if (device.value(QStringLiteral("included")).toBool() && !device.value("cached").toBool())
            includedDevices.append(device);
    }

    if (m_includedDevices == includedDevices)
        return;
    m_includedDevices = std::move(includedDevices);
    emit includedDevicesChanged();
}

void TailscaleManager::startPrivilegedAction(const QString &program,
                                              const QStringList &arguments,
                                              Action action)
{
    if (m_actionProcess.state() != QProcess::NotRunning || m_statusProcess.state() != QProcess::NotRunning)
        return;

    setBusy(true);
    setErrorMessage({});
    m_action = action;
    m_timeout.start(action == Action::Login ? 310000 : 15000);
    m_actionOutput.clear();
    m_loginUrl.clear();

    const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (pkexec.isEmpty()) {
        m_actionProcess.start(program, arguments);
        return;
    }

    QStringList privilegedArguments{program};
    privilegedArguments.append(arguments);
    m_actionProcess.start(pkexec, privilegedArguments);
}

void TailscaleManager::readActionOutput()
{
    m_actionOutput.append(m_actionProcess.readAllStandardOutput());
    if (m_action != Action::Login)
        return;

    static const QRegularExpression loginUrlExpression(QStringLiteral(R"((https://[^\s]+))"));
    const QRegularExpressionMatch match = loginUrlExpression.match(QString::fromUtf8(m_actionOutput));
    if (!match.hasMatch())
        return;

    const QString url = match.captured(1);
    if (url == m_loginUrl)
        return;

    m_loginUrl = url;
    QDesktopServices::openUrl(QUrl(url));
}

void TailscaleManager::handleStatusFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    const QByteArray output = m_statusProcess.readAllStandardOutput();
    const QString error = QString::fromUtf8(m_statusProcess.readAllStandardError()).trimmed();
    applyStatus(output, error, exitStatus == QProcess::NormalExit && exitCode == 0);
}

void TailscaleManager::clearPeerSnapshots()
{
    NetworkSnapshot::clear("tailscale-discovery");
    NetworkSnapshot::clear("tailscale-status");
    m_discoveryDevices.clear();
    setDevices({});
}

void TailscaleManager::showSavedPeers()
{
    const auto identity = PilonSettings().value("tailscale/usedIdentity").toString();
    const auto file = identity.isEmpty() ? QString{} : NetworkSnapshot::path("tailscale-status", {identity});
    auto peers = NetworkSnapshot::read(file).value("devices").toArray().toVariantList();
    for (auto &entry : peers) {
        auto peer = entry.toMap();
        peer.insert("cached", true);
        peer.insert("onlineKnown", false);
        peer.insert("online", false);
        entry = peer;
    }
    setDevices(peers);
}

void TailscaleManager::applyStatus(const QByteArray &output, const QString &error, bool success, bool serviceAvailable)
{
    m_timeout.stop();

    if (!success) {
        m_cleanupPending=false;
        setServiceRunning(false);
        setLoggedIn(false);
#ifndef Q_OS_ANDROID
        setVpnDisconnected(true);
#endif
        showSavedPeers();
        if (!error.isEmpty()) setErrorMessage(error);
        else if (m_errorMessage.isEmpty()) setErrorMessage(tr("Unable to read the device account status. Check the service and try again."));
        setBusy(false);
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        m_cleanupPending=false;
        setServiceRunning(serviceAvailable);
        setLoggedIn(false);
        showSavedPeers();
        setErrorMessage(tr("Tailscale returned an unreadable device status."));
        setBusy(false);
        return;
    }

    const QJsonObject status = document.object();
    const auto selfIdentity=status.value("Self").toObject();
    m_currentIdentity=selfIdentity.value("ID").toString()+"/"+selfIdentity.value("UserID").toVariant().toString();
    if (selfIdentity.value("ID").toString().isEmpty()) m_currentIdentity.clear();
    const QString backendState = status.value(QStringLiteral("BackendState")).toString();
    if (backendState.isEmpty()) {
        m_cleanupPending=false;
        setErrorMessage(tr("Tailscale returned an unreadable account status."));setBusy(false);return;
    }
    const bool needsLogin = backendState == QLatin1String("NoState")
                            || backendState == QLatin1String("NeedsLogin");

    QVariantList devices;
    const QJsonObject self = status.value(QStringLiteral("Self")).toObject();
    if (!self.isEmpty())
        devices.append(deviceDetails(self, QStringLiteral("self"), true));

    const QJsonObject peers = status.value(QStringLiteral("Peer")).toObject();
    for (auto peer = peers.constBegin(); peer != peers.constEnd(); ++peer)
        devices.append(deviceDetails(peer.value().toObject(), peer.key(), false));

    std::sort(devices.begin(), devices.end(), [](const QVariant &left, const QVariant &right) {
        return QString::localeAwareCompare(left.toMap().value(QStringLiteral("name")).toString(),
                                           right.toMap().value(QStringLiteral("name")).toString()) < 0;
    });

    PilonSettings settings;
    bool selectionInitialized = settings.value(QLatin1String(deviceSelectionInitializedKey), false).toBool();
    QStringList includedIds = settings.value(QLatin1String(includedDevicesKey)).toStringList();

    for (QVariant &deviceValue : devices) {
        QVariantMap device = deviceValue.toMap();
        device.insert(QStringLiteral("included"),
                      selectionInitialized
                          && includedIds.contains(device.value(QStringLiteral("id")).toString()));
        deviceValue = device;
    }

    if (!needsLogin && m_currentIdentity.isEmpty()) {
        m_cleanupPending=false;
        setErrorMessage(tr("Tailscale did not identify the signed-in account."));setBusy(false);return;
    }
    setServiceRunning(serviceAvailable);
    setLoggedIn(!needsLogin);
#ifndef Q_OS_ANDROID
    if (!serviceAvailable || backendState != QLatin1String("Running")) setVpnDisconnected(true);
#endif
    if (!needsLogin && settings.value("tailscale/usedIdentity").toString() == m_currentIdentity)
        NetworkSnapshot::write(NetworkSnapshot::path("tailscale-status", {m_currentIdentity}),
                               {{"devices", QJsonArray::fromVariantList(devices)}});
    else NetworkSnapshot::clear("tailscale-status");
    setDevices(std::move(devices));
#ifndef Q_OS_ANDROID
    if (serviceAvailable && backendState == QLatin1String("Running")) setVpnDisconnected(false);
#endif
    setStatusKnown(true);
    setBusy(false);
    if (m_claimAfterLogin && m_loggedIn) {m_claimAfterLogin=false;useCurrentAccount();}
    if (m_cleanupPending) finishUsedAccountCleanup();
}

void TailscaleManager::handleActionFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    readActionOutput();
    const QString output = QString::fromUtf8(m_actionOutput).trimmed();
    const bool succeeded = exitStatus == QProcess::NormalExit && exitCode == 0;
    const auto action=m_action;
    m_timeout.stop();
    m_action = Action::None;
    setBusy(false);

    if (!succeeded) {
        setErrorMessage(output.isEmpty() ? tr("The Tailscale action failed.") : output);
        return;
    }

    if (action == Action::Logout && !forgetUseReceipt()) return;
    if (action == Action::Login) m_claimAfterLogin=true;
    refresh();
}

#ifdef Q_OS_ANDROID
void TailscaleManager::refreshExternalApplication()
{
    const auto context = QNativeInterface::QAndroidApplication::context();
    const auto encoded = QJniObject::callStaticObjectMethod("org/pilon/PilonTailscaleExternal", "status",
        "(Landroid/content/Context;)Ljava/lang/String;", context.object()).toString();
    QJsonParseError parse{};
    const auto result = QJsonDocument::fromJson(encoded.toUtf8(), &parse);
    const auto status = result.object();
    const auto failure = parse.error != QJsonParseError::NoError || !result.isObject()
        ? tr("Unable to read the Android VPN status.") : status.value("error").toString();
    if (!failure.isEmpty()) { setStatusKnown(false); setErrorMessage(failure); return; }
    const bool installed = status.value("installed").toBool();
    const bool vpn = status.value("vpnAvailable").toBool();
    setVpnDisconnected(!installed || !status.value("enabled").toBool() || !vpn);
    const QString description = !installed ? tr("Tailscale is not installed.")
        : !status.value("enabled").toBool() ? tr("Tailscale is disabled. Open it to enable it in Android settings.")
        : status.value("pilonVpnActive").toBool() ? tr("Pilon's previous VPN is running. Connect to switch to the Tailscale app.")
        : vpn ? tr("Android VPN active. Check Tailscale for its connection status.") : tr("No Android VPN active.");
    if (installed != m_externalAppInstalled || description != m_externalStatus) {
        m_externalAppInstalled = installed; m_externalStatus = description; emit externalStatusChanged();
    }
    QString savedError;
    loadSavedDevices(&savedError);
    mergeExternalDevices(); setLoggedIn(false); setServiceRunning(vpn); m_currentIdentity.clear();
    setStatusKnown(true);
    if (!savedError.isEmpty()) setErrorMessage(savedError);
}

void TailscaleManager::externalAction(const QString &action)
{
    if (m_busy) { setErrorMessage(tr("Wait for the device operation to finish.")); return; }
    setErrorMessage({});
    const auto context = QNativeInterface::QAndroidApplication::context();
    const auto operation = QJniObject::fromString(action);
    const auto error = QJniObject::callStaticObjectMethod("org/pilon/PilonTailscaleExternal", "operate",
        "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;", context.object(), operation.object<jstring>()).toString();
    refreshExternalApplication();
    if (!error.isEmpty()) setErrorMessage(error);
}

void TailscaleManager::startAndroidAction(Action action)
{
    if (m_busy) return;
    setBusy(true);
    setErrorMessage({});
    m_action = action;
    m_loginUrl.clear();
    const auto operation = action == Action::None ? "status" : action == Action::StartService ? "start" : action == Action::Login ? "login" : "logout";
    const auto context = QNativeInterface::QAndroidApplication::context();
    const auto name = QJniObject::fromString(QLatin1String(operation));
    m_androidRequest = QJniObject::callStaticMethod<jlong>("org/pilon/PilonTailscale", "begin",
        "(Landroid/content/Context;Ljava/lang/String;)J", context.object(), name.object<jstring>());
    if (!m_androidRequest) {
        m_action = Action::None;
        setStatusKnown(false);
        setErrorMessage(tr("Unable to start the Tailscale operation. Reinstall Pilon if this continues."));
        setBusy(false);
        return;
    }
    m_timeout.start(action == Action::Login || action == Action::StartService ? 325000 : 15000);
    m_androidPoll.start();
}

void TailscaleManager::cancelAndroid()
{
    m_androidPoll.stop();
    if (m_androidRequest) QJniObject::callStaticMethod<void>("org/pilon/PilonTailscale", "cancel", "(J)V", jlong(m_androidRequest));
    m_androidRequest = 0;
}

void TailscaleManager::pollAndroid()
{
    if (!m_androidRequest) return;
    const auto data = QJniObject::callStaticObjectMethod("org/pilon/PilonTailscale", "poll", "(J)Ljava/lang/String;", jlong(m_androidRequest)).toString();
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(data.toUtf8(), &error);
    const auto result = document.object();
    if (result.value("pending").toBool()) {
        const auto url = result.value("loginUrl").toString();
        if (m_action == Action::Login && !url.isEmpty() && url != m_loginUrl && QUrl(url).scheme() == "https") {
            m_loginUrl = url;
            if (!QDesktopServices::openUrl(QUrl(url))) setErrorMessage(tr("Unable to open the Tailscale sign-in page."));
        }
        return;
    }
    const auto action = m_action;
    cancelAndroid();
    m_action = Action::None;
    m_timeout.stop();
    const auto failure = error.error != QJsonParseError::NoError || !document.isObject()
        ? tr("Tailscale returned an unreadable operation result.") : result.value("error").toString();
    if (!failure.isEmpty()) {
        if (action == Action::None) applyStatus({}, failure, false, false);
        else { setErrorMessage(failure); setBusy(false); }
        return;
    }
    if (action == Action::Logout) {
        if (!forgetUseReceipt()) { setBusy(false); return; }
        setBusy(false); refreshExternalApplication(); return;
    }
    if (action == Action::Login) m_claimAfterLogin = true;
    applyStatus(QJsonDocument(result.value("status").toObject()).toJson(), {}, true, result.value("serviceRunning").toBool());
}
#endif
