#include "tailscalemanager.h"
#include "../../../src/core/network/networksnapshot.h"
#include "../../../src/platform/android/androidcredentials.h"
#include "../../../src/core/workspace/pilonsettings.h"

#include <QDateTime>
#include <QFutureWatcher>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QtConcurrentRun>
#include <algorithm>

namespace {
constexpr auto scope = "devices:core:read";
constexpr qsizetype maximumResponse = 4 * 1024 * 1024;
QMutex credentialMutex;
struct SecretResult { bool ok = false; QByteArray secret; bool recoveryFailed = false; };

QString discoverySnapshot(const QByteArray &credential)
{
    const auto id = QJsonDocument::fromJson(credential).object().value("clientId").toString();
    return id.isEmpty() ? QString{} : NetworkSnapshot::path("tailscale-discovery", {id});
}

bool credentialValid(const QJsonObject &credential)
{
    static const QRegularExpression id(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    static const QRegularExpression secret(QStringLiteral("^[A-Za-z0-9_-]{16,512}$"));
    return id.match(credential.value("clientId").toString()).hasMatch()
        && secret.match(credential.value("clientSecret").toString()).hasMatch();
}

SecretResult keyring(const QString &action, const QByteArray &secret)
{
#ifdef Q_OS_ANDROID
    const auto result = AndroidCredentials::request(action,
        {{"application", "pilon-tailscale-discovery"}, {"connection", "default"}}, secret);
    return {result.ok && result.secret.size() <= 4096, result.secret};
#else
    const auto tool = QStandardPaths::findExecutable("secret-tool");
    if (tool.isEmpty()) return {false, {}};
    QStringList arguments{action};
    if (action == "store") arguments << "--label=Pilon Tailscale discovery";
    arguments << "application" << "pilon-tailscale-discovery" << "connection" << "default";
    QProcess process;
    QByteArray output;
    qsizetype errors = 0;
    bool oversized = false;
    QObject::connect(&process, &QProcess::readyReadStandardOutput, &process, [&] {
        output += process.readAllStandardOutput();
        if (output.size() > 4096) { oversized = true; process.kill(); }
    });
    QObject::connect(&process, &QProcess::readyReadStandardError, &process, [&] {
        errors += process.readAllStandardError().size();
        if (errors > 4096) { oversized = true; process.kill(); }
    });
    process.start(tool, arguments);
    if (!process.waitForStarted(3000)) return {false, {}};
    if (action == "store") process.write(secret);
    process.closeWriteChannel();
    if (!process.waitForFinished(5000)) { process.kill(); process.waitForFinished(1000); return {false, {}}; }
    const bool absent = action != "store" && process.exitCode() == 1 && errors == 0;
    return {!oversized && process.exitStatus() == QProcess::NormalExit && (process.exitCode() == 0 || absent),
        absent ? QByteArray() : output.trimmed()};
#endif
}
}

void TailscaleManager::discoverySecret(const QString &action, const QByteArray &secret,
                                      std::function<void(bool, const QByteArray &)> completed)
{
    const auto generation = m_discoveryGeneration;
    const auto state = m_discoveryCommitState;
    const auto previous = m_discoveryCredential;
    auto *watcher = new QFutureWatcher<SecretResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, completed] {
        const auto result = watcher->result(); watcher->deleteLater();
        if (result.recoveryFailed) {
            if (generation != m_discoveryGeneration && (m_discoveryGeneration != generation + 1 || m_busy)) return;
            m_discoveryConnected = false; m_discoveryDevices.clear(); m_discoveryToken.clear();
            mergeExternalDevices(); emit discoveryChanged();
            const auto error = tr("The secure store could not restore device discovery. Reconnect device discovery.");
            if (generation == m_discoveryGeneration) finishDiscovery(error); else setErrorMessage(error);
            return;
        }
        if (generation == m_discoveryGeneration) completed(result.ok, result.secret);
    });
    watcher->setFuture(QtConcurrent::run([action, secret, previous, state] {
        QMutexLocker lock(&credentialMutex);
        if (state && state->load() == DiscoveryCommit::Cancelled) return SecretResult{};
        auto result = keyring(action, secret);
        if (action != "lookup" && result.ok && state) {
            // Cancellation restores the old record before another keyring request can run.
            auto active = DiscoveryCommit::Active;
            if (!state->compare_exchange_strong(active, DiscoveryCommit::Committed)) {
                const auto restored = keyring(previous.isEmpty() ? "clear" : "store", previous);
                if (!restored.ok) keyring("clear", {});
                return SecretResult{false, {}, !restored.ok};
            }
        }
        return result;
    }));
}

bool TailscaleManager::cancelDiscoveryRequest()
{
    if (m_discoveryCommitState) {
        auto active = DiscoveryCommit::Active;
        if (!m_discoveryCommitState->compare_exchange_strong(active, DiscoveryCommit::Cancelled) && active == DiscoveryCommit::Committed) return false;
    }
    ++m_discoveryGeneration;
    if (m_discoveryReply) {
        disconnect(m_discoveryReply, nullptr, this, nullptr);
        m_discoveryReply->abort(); m_discoveryReply->deleteLater(); m_discoveryReply = nullptr;
    }
    return true;
}

void TailscaleManager::markDiscoveryDevicesCached()
{
    for (auto &entry : m_discoveryDevices) {
        auto device = entry.toMap();
        device.insert("cached", true);
        entry = device;
    }
}

void TailscaleManager::finishDiscovery(const QString &error, bool accessDenied)
{
    m_timeout.stop();
    m_discoveryCommitState.reset();
    if (!error.isEmpty()) {
        m_discoveryToken.clear();
        m_discoveryTokenExpiry = 0;
        if (m_refreshingDiscovery) {
            if (accessDenied) {
                const auto file = discoverySnapshot(m_discoveryCredential);
                if (!file.isEmpty()) QFile::remove(file);
                m_discoveryDevices.clear();
            } else {
                markDiscoveryDevicesCached();
            }
            mergeExternalDevices();
            emit discoveryChanged();
        }
    }
    m_refreshingDiscovery = false;
    setErrorMessage(error);
    setBusy(false);
}

void TailscaleManager::connectDiscovery(const QString &clientId, const QString &clientSecret)
{
    if (m_busy) { setErrorMessage(tr("Wait for the device operation to finish.")); return; }
    const QJsonObject credential{{"clientId", clientId.trimmed()}, {"clientSecret", clientSecret.trimmed()}};
    if (!credentialValid(credential)) { setErrorMessage(tr("Enter the OAuth client ID and client secret.")); return; }
    cancelDiscoveryRequest(); setErrorMessage({}); setBusy(true); m_timeout.start(15000);
    m_discoveryCommitState = std::make_shared<std::atomic<DiscoveryCommit>>(DiscoveryCommit::Active);
    const auto bytes = QJsonDocument(credential).toJson(QJsonDocument::Compact);
    discoverySecret("lookup", {}, [this, bytes](bool ok, const QByteArray &previous) {
        if (!ok) { finishDiscovery(tr("The secure credential store could not be opened. Unlock it and retry.")); return; }
        m_discoveryCredential = previous;
        requestDiscoveryToken(bytes, true);
    });
}

void TailscaleManager::restoreDiscovery()
{
    if (m_busy) return;
    cancelDiscoveryRequest(); setBusy(true); m_timeout.start(15000);
    m_discoveryCommitState = std::make_shared<std::atomic<DiscoveryCommit>>(DiscoveryCommit::Active);
    discoverySecret("lookup", {}, [this](bool ok, const QByteArray &credential) {
        if (!ok) { finishDiscovery(tr("The secure credential store could not be opened. Unlock it and retry.")); return; }
        m_discoveryConnected = !credential.isEmpty() && credentialValid(QJsonDocument::fromJson(credential).object());
        m_discoveryCredential = m_discoveryConnected ? credential : QByteArray();
        m_discoveryDevices = NetworkSnapshot::read(discoverySnapshot(m_discoveryCredential))
                                 .value("devices").toArray().toVariantList();
        markDiscoveryDevicesCached();
        mergeExternalDevices();
        finishDiscovery(credential.isEmpty() || m_discoveryConnected ? QString() : tr("The saved discovery credential is unreadable. Connect device discovery again."));
        emit discoveryChanged();
    });
}

void TailscaleManager::refreshDiscovery()
{
    if (m_busy || !m_discoveryConnected) return;
    cancelDiscoveryRequest(); setBusy(true); m_timeout.start(15000);
    m_refreshingDiscovery = true;
    m_discoveryCommitState = std::make_shared<std::atomic<DiscoveryCommit>>(DiscoveryCommit::Active);
    m_discoveryRefreshAt = QDateTime::currentMSecsSinceEpoch() + 60000;
    discoverySecret("lookup", {}, [this](bool ok, const QByteArray &credential) {
        if (!ok) { finishDiscovery(tr("The secure credential store could not be opened. Unlock it and retry.")); return; }
        if (!credentialValid(QJsonDocument::fromJson(credential).object())) {
            m_discoveryConnected = false;
            finishDiscovery(tr("The saved discovery credential could not be read. Connect device discovery again."), true);
            m_discoveryCredential.clear();
            return;
        }
        if (credential != m_discoveryCredential) {
            m_discoveryToken.clear();
            m_discoveryDevices.clear();
            mergeExternalDevices();
        }
        m_discoveryCredential = credential;
        if (!m_discoveryToken.isEmpty() && QDateTime::currentMSecsSinceEpoch() < m_discoveryTokenExpiry)
            requestDiscoveryDevices(credential, false);
        else requestDiscoveryToken(credential, false);
    });
}

void TailscaleManager::requestDiscoveryToken(const QByteArray &credential, bool save)
{
    const auto object = QJsonDocument::fromJson(credential).object();
    QUrlQuery form;
    form.addQueryItem("grant_type", "client_credentials");
    form.addQueryItem("client_id", object.value("clientId").toString());
    form.addQueryItem("client_secret", object.value("clientSecret").toString());
    form.addQueryItem("scope", scope);
    discoveryRequest("/api/v2/oauth/token", form.toString(QUrl::FullyEncoded).toUtf8(), {}, [this, credential, save](const QJsonObject &result) {
        const auto token = result.value("access_token").toString().toLatin1();
        static const QRegularExpression bearer(QStringLiteral("^[A-Za-z0-9._~+/-]+=*$"));
        const auto granted = result.value("scope").toString();
        if (token.isEmpty() || token.size() > 4096 || !bearer.match(QString::fromLatin1(token)).hasMatch()
            || result.value("token_type").toString().compare("Bearer", Qt::CaseInsensitive) != 0
            || (!granted.isEmpty() && granted.trimmed() != QLatin1String(scope))) {
            finishDiscovery(tr("Tailscale did not grant read-only device access. Use devices:core:read and reconnect."), true); return;
        }
        m_discoveryToken = token;
        m_discoveryTokenExpiry = QDateTime::currentMSecsSinceEpoch() + qBound(1, result.value("expires_in").toInt(3600), 3600) * 1000LL - 30000;
        requestDiscoveryDevices(credential, save);
    });
}

void TailscaleManager::discoveryRequest(const QString &path, const QByteArray &body, const QByteArray &token,
                                       std::function<void(const QJsonObject &)> completed)
{
    if (!m_discoveryNetwork) m_discoveryNetwork = std::make_unique<QNetworkAccessManager>();
    QNetworkRequest request(QUrl("https://api.tailscale.com" + path));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(15000);
    request.setRawHeader("Accept", "application/json");
    if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token);
    if (!body.isEmpty()) request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    auto *reply = body.isEmpty() ? m_discoveryNetwork->get(request) : m_discoveryNetwork->post(request, body);
    m_discoveryReply = reply;
    reply->setReadBufferSize(65536);
    m_timeout.start(15000);
    const auto generation = m_discoveryGeneration;
    auto bytes = std::make_shared<QByteArray>();
    auto oversized = std::make_shared<bool>(false);
    connect(reply, &QIODevice::readyRead, this, [reply, bytes, oversized] {
        bytes->append(reply->readAll());
        if (bytes->size() > maximumResponse) { *oversized = true; reply->abort(); }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, oversized, generation, completed] {
        reply->deleteLater();
        if (generation != m_discoveryGeneration || m_discoveryReply != reply) return;
        m_discoveryReply = nullptr;
        bytes->append(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 400 || status == 401 || status == 403) {
            finishDiscovery(tr("Device discovery was denied. Check the OAuth client ID, secret and devices:core:read permission."), true); return;
        }
        if (*oversized || bytes->size() > maximumResponse) { finishDiscovery(tr("Tailscale returned too much device data.")); return; }
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            finishDiscovery(tr("Unable to read Tailscale devices. Check the connection and refresh.")); return;
        }
        QJsonParseError parse{};
        const auto document = QJsonDocument::fromJson(*bytes, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) {
            finishDiscovery(tr("Tailscale returned unreadable discovery data.")); return;
        }
        completed(document.object());
    });
}

void TailscaleManager::requestDiscoveryDevices(const QByteArray &credential, bool save)
{
    discoveryRequest("/api/v2/tailnet/-/devices", {}, m_discoveryToken, [this, credential, save](const QJsonObject &result) {
        QVariantList devices;
        QSet<QString> ids;
        static const QRegularExpression identifier(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
        static const QRegularExpression control(QStringLiteral("[\\x{0000}-\\x{001f}\\x{007f}]"));
        if (!result.value("devices").isArray() || result.value("devices").toArray().size() > 10000) {
            finishDiscovery(tr("Tailscale returned an unreadable device list.")); return;
        }
        const auto local = QNetworkInterface::allAddresses();
        for (const auto &entry : result.value("devices").toArray()) {
            const auto device = entry.toObject();
            const auto id = device.value("nodeId").toString(device.value("id").toString());
            auto name = device.value("hostname").toString();
            if (name.isEmpty()) name = device.value("name").toString();
            QString address;
            bool self = false, valid = entry.isObject() && identifier.match(id).hasMatch() && !ids.contains(id)
                && !name.isEmpty() && name.size() <= 253 && !control.match(name).hasMatch() && device.value("addresses").isArray();
            for (const auto &value : device.value("addresses").toArray()) {
                QHostAddress ip;
                if (!value.isString() || !ip.setAddress(value.toString()) || ip.isNull() || ip.isLoopback() || ip.isMulticast() || !ip.scopeId().isEmpty()) { valid = false; break; }
                self = self || local.contains(ip);
                if (address.isEmpty() || ip.protocol() == QAbstractSocket::IPv4Protocol) address = ip.toString();
            }
            if (!valid || address.isEmpty()) { finishDiscovery(tr("Tailscale returned an unreadable device list.")); return; }
            ids.insert(id);
            devices.append(QVariantMap{{"id", "api:" + id}, {"name", name}, {"ip", address},
                {"online", false}, {"onlineKnown", false}, {"self", self}, {"saved", false}});
        }
        const auto accept = [this, devices] {
            m_discoveryDevices = devices; m_discoveryConnected = true;
            NetworkSnapshot::write(discoverySnapshot(m_discoveryCredential),
                                   {{"devices", QJsonArray::fromVariantList(devices)}});
            m_discoveryRefreshAt = QDateTime::currentMSecsSinceEpoch() + 60000;
            mergeExternalDevices(); finishDiscovery(); emit discoveryChanged();
        };
        if (!save) { accept(); return; }
        discoverySecret("store", credential, [this, credential, accept](bool ok, const QByteArray &) {
            if (!ok) { finishDiscovery(tr("The secure credential store could not save device discovery. Unlock it and retry.")); return; }
            m_discoveryCredential = credential; accept();
        });
    });
}

void TailscaleManager::mergeExternalDevices()
{
    auto devices = loadSavedDevices();
    QSet<QString> addresses;
    for (const auto &entry : devices) addresses.insert(entry.toMap().value("ip").toString());
    const auto included = PilonSettings().value("tailscale/includedDevices").toStringList();
    for (const auto &entry : m_discoveryDevices) {
        auto device = entry.toMap();
        if (addresses.contains(device.value("ip").toString())) continue;
        device.insert("included", included.contains(device.value("id").toString()));
        devices.append(device);
    }
    std::sort(devices.begin(), devices.end(), [](const auto &left, const auto &right) {
        return QString::localeAwareCompare(left.toMap().value("name").toString(), right.toMap().value("name").toString()) < 0;
    });
    setDevices(devices);
}

void TailscaleManager::disconnectDiscovery()
{
    if (m_busy) { setErrorMessage(tr("Wait for the device operation to finish.")); return; }
    cancelDiscoveryRequest(); setBusy(true); m_timeout.start(15000);
    m_discoveryCommitState = std::make_shared<std::atomic<DiscoveryCommit>>(DiscoveryCommit::Active);
    discoverySecret("lookup", {}, [this](bool ok, const QByteArray &previous) {
        if (!ok) { finishDiscovery(tr("The secure credential store could not be opened. Unlock it and retry.")); return; }
        m_discoveryCredential = previous;
        discoverySecret("clear", {}, [this](bool cleared, const QByteArray &) {
            if (!cleared) { finishDiscovery(tr("The secure credential store could not remove device discovery. Unlock it and retry.")); return; }
            NetworkSnapshot::clear("tailscale-discovery");
            m_discoveryConnected = false; m_discoveryDevices.clear(); m_discoveryCredential.clear(); m_discoveryToken.clear();
            mergeExternalDevices(); finishDiscovery(); emit discoveryChanged();
        });
    });
}
