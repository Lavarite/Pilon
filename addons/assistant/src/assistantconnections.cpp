#include "assistantconnections.h"
#include "../../../src/core/network/networksnapshot.h"
#include "../../../src/platform/android/androidcredentials.h"
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include "../../../src/core/workspace/pilonsettings.h"
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace {
QString accountQuotaPath(const QString &home) {
    return home.isEmpty() ? QString{} : NetworkSnapshot::path("assistant-account-usage", {home});
}
QString accountIdentity(const QVariantMap &account) {
    const auto email = account.value("email").toString().trimmed();
    if (email.isEmpty() || account.value("type") != "chatgpt") return {};
    const QJsonArray identity{email, account.value("accountId").toString(), account.value("planType").toString()};
    return NetworkSnapshot::digest(QJsonDocument(identity).toJson(QJsonDocument::Compact));
}
QString serverQuotaPath(const QVariantMap &profile) {
    return NetworkSnapshot::path("assistant-server-usage", {profile.value("id").toString(),
        profile.value("baseUrl").toString(), profile.value("remoteModel").toString(),
        profile.value("protocol").toString(), profile.value("quotaRevision").toString()});
}
}

AssistantConnections::AssistantConnections(QObject *parent) : QObject(parent)
{
    m_servers = PilonSettings().value("assistant/apiServers").toList();
    for (const auto &value : m_servers) {
        const auto server = value.toMap();
        const auto snapshot = NetworkSnapshot::read(serverQuotaPath(server));
        if (!snapshot.isEmpty()) m_serverUsage[server.value("id").toString()] = snapshot.toVariantMap();
    }
    if (PilonSettings().value("assistant/chatgptEnabled", false).toBool()) m_models = PilonSettings().value("assistant/chatgptModels").toList();
    for (auto &value : m_models) {
        auto model = value.toMap(); auto name = model.value("name").toString();
        if (name.endsWith(" (ChatGPT)")) { name.chop(10); model["name"] = name; value = model; }
    }
    connect(&m_client, &CodexClient::failed, this, [this](const QString &error) {
        ++m_loginGeneration; ++m_accountGeneration; m_loginId.clear(); m_userCode.clear(); m_verificationUrl.clear();
        for (const auto &connection : m_pendingStarts) disconnect(connection);
        m_pendingStarts.clear(); m_signingIn = false; m_signingOut = false; fail(error);
    });
    connect(&m_client, &CodexClient::notification, this, [this](const QString &method, const QJsonObject &params) {
        if (method == "account/login/completed") {
            if (!m_signingIn || m_loginId.isEmpty() || params.value("loginId").toString() != m_loginId) return;
            const auto generation = ++m_loginGeneration;
            m_signingIn = false; m_loginId.clear(); m_userCode.clear(); m_verificationUrl.clear(); emit stateChanged();
            if (generation != m_loginGeneration) return;
            if (!params.value("success").toBool()) { fail(params.value("error").toString()); return; }
            PilonSettings().setValue("assistant/chatgptEnabled", true);
            refreshAccount(); emit authenticationChanged();
        } else if (method == "account/updated") refreshAccount();
        else if (method == "account/rateLimits/updated") updateRateLimits(params);
    });
}
void AssistantConnections::configure(const QString &executable, const QString &home, bool refresh)
{
    if (!m_executable.isEmpty() && (m_executable != executable || m_home != home)) {
        ++m_loginGeneration; ++m_accountGeneration;
        m_loginId.clear(); m_userCode.clear(); m_verificationUrl.clear(); m_signingIn = false; m_signingOut = false;
        for (const auto &connection : std::as_const(m_pendingStarts)) disconnect(connection);
        m_pendingStarts.clear(); m_client.shutdown();
        if (m_home != home) { m_account.clear(); m_rateLimits = {}; m_models.clear(); emit modelsChanged(); }
        emit stateChanged();
    }
    m_executable = executable; m_home = home;
    if (refresh && PilonSettings().value("assistant/chatgptEnabled", false).toBool()) refreshAccount();
}
void AssistantConnections::suspend()
{
    if (m_signingIn || m_signingOut) return;
    const bool refreshing = refreshingUsage();
    ++m_accountGeneration; ++m_loginGeneration;
    for (const auto &connection : std::as_const(m_pendingStarts)) disconnect(connection);
    m_pendingStarts.clear(); m_client.shutdown();
    if (refreshing) emit stateChanged();
}
void AssistantConnections::fail(const QString &error) { m_error = error; emit stateChanged(); }
bool AssistantConnections::hasChatGptSession() const {
    return !m_account.isEmpty() || PilonSettings().value("assistant/chatgptEnabled", false).toBool();
}
void AssistantConnections::request(const QString &method, const QJsonObject &params, CodexClient::Reply reply)
{
    if (m_client.ready()) { m_client.request(method, params, std::move(reply)); return; }
    if (m_executable.isEmpty()) { m_signingIn = false; m_signingOut = false; fail(tr("Codex is unavailable. Install the Codex runtime to manage this account.")); return; }
    if (!m_client.running() && (!QDir().mkpath(m_home)
            || !QFile::setPermissions(m_home, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))) {
        ++m_loginGeneration; m_loginId.clear(); m_signingIn = false; m_signingOut = false;
        fail(tr("The private Codex account folder could not be prepared.")); return;
    }
    m_pendingStarts.append(connect(&m_client, &CodexClient::initialized, this, [this, method, params, reply] { m_client.request(method, params, reply); }, Qt::SingleShotConnection));
    if (m_client.running()) return;
    auto environment = QProcessEnvironment::systemEnvironment(); environment.insert("CODEX_HOME", m_home);
    // Never inherit API credentials in the subscription account process.
    environment.remove("OPENAI_API_KEY"); environment.remove("OPENAI_BASE_URL"); environment.remove("CODEX_API_KEY"); environment.remove("CODEX_ACCESS_TOKEN"); environment.remove("CODEX_AUTH_TOKEN");
    m_client.start(m_executable, {"app-server", "--listen", "stdio://", "-c", "model_provider=\"openai\"", "-c", "cli_auth_credentials_store=\"auto\""}, environment);
}
void AssistantConnections::signIn()
{
#ifdef Q_OS_ANDROID
    startSignIn(true);
#else
    startSignIn(false);
#endif
}
void AssistantConnections::continueSignIn()
{
    if (m_signingIn && !m_loginId.isEmpty() && !m_userCode.isEmpty() && !m_verificationUrl.isEmpty()) emit loginUrl(m_verificationUrl);
}
void AssistantConnections::startSignIn(bool deviceCode)
{
    if (m_signingIn || m_signingOut) return;
    const auto generation = ++m_loginGeneration;
    ++m_accountGeneration;
    m_error.clear(); m_loginId.clear(); m_userCode.clear(); m_verificationUrl.clear(); m_signingIn = true; emit stateChanged();
    if (generation != m_loginGeneration || !m_signingIn) return;
    request("account/login/start", {{"type", deviceCode ? "chatgptDeviceCode" : "chatgpt"}}, [this, generation, deviceCode](QJsonObject result, QString error) {
        if (generation != m_loginGeneration || !m_signingIn) return;
        if (!error.isEmpty()) {
            if (deviceCode && error.contains("device code login is not enabled", Qt::CaseInsensitive))
                error = tr("Enable device code login in your ChatGPT security settings or ask your workspace administrator to allow it, then try again.");
            m_signingIn = false; fail(error); return;
        }
        m_loginId = result.value("loginId").toString();
        if (deviceCode) {
            const auto code = result.value("userCode").toString();
            const auto printable = [](QChar character) {
                return character.isPrint() && character.category() != QChar::Other_Format && character.category() != QChar::Other_Surrogate
                    && character.category() != QChar::Separator_Line && character.category() != QChar::Separator_Paragraph;
            };
            const bool validId = !m_loginId.isEmpty() && m_loginId.size() <= 128
                && std::all_of(m_loginId.cbegin(), m_loginId.cend(), printable);
            if (!validId) m_loginId.clear();
            if (!validId || result.value("type") != "chatgptDeviceCode" || code.isEmpty() || code.size() > 64 || code.trimmed() != code
                    || !std::all_of(code.cbegin(), code.cend(), printable)
                    || result.value("verificationUrl").toString() != "https://auth.openai.com/codex/device") {
                m_error = tr("Codex returned an invalid device sign-in response."); cancelSignIn(); return;
            }
            m_userCode = code; m_verificationUrl = QUrl("https://auth.openai.com/codex/device"); emit stateChanged(); return;
        }
        const QUrl url(result.value("authUrl").toString());
        if (m_loginId.isEmpty() || !url.isValid() || url.scheme() != "https" || url.host().isEmpty() || !url.userInfo().isEmpty()) {
            m_error = tr("Codex returned an invalid sign-in response."); cancelSignIn(); return;
        }
        emit loginUrl(url); emit stateChanged();
    });
}
void AssistantConnections::cancelSignIn()
{
    ++m_loginGeneration;
    const auto loginId = std::exchange(m_loginId, {});
    const auto wasSigningIn = std::exchange(m_signingIn, false);
    m_userCode.clear(); m_verificationUrl.clear();
    if (!loginId.isEmpty()) request("account/login/cancel", {{"loginId", loginId}}, {});
    else if (wasSigningIn) {
        for (const auto &connection : m_pendingStarts) disconnect(connection); m_pendingStarts.clear();
        m_client.shutdown();
    }
    emit stateChanged();
}
void AssistantConnections::signOut()
{
    if (m_signingOut) return;
    cancelSignIn();
    const auto generation = ++m_accountGeneration;
    m_signingOut = true; m_error.clear(); emit stateChanged();
    request("account/logout", {}, [this, generation](QJsonObject, QString error) {
        if (generation != m_accountGeneration) return;
        m_signingOut = false;
        if (!error.isEmpty()) { fail(error); return; }
        QFile::remove(accountQuotaPath(m_home));
        ++m_accountGeneration; m_account.clear(); m_rateLimits = {}; m_models.clear(); m_error.clear();
        PilonSettings().remove("assistant/chatgptEnabled"); PilonSettings().remove("assistant/chatgptModels");
        emit modelsChanged(); emit stateChanged(); emit authenticationChanged();
    });
}
void AssistantConnections::refreshAccount()
{
    if (m_signingOut) return;
    const auto generation = ++m_accountGeneration;
    request("account/read", {{"refreshToken", false}}, [this, generation](QJsonObject result, QString error) {
        if (generation != m_accountGeneration) return;
        if (!error.isEmpty()) { fail(error); return; }
        const auto value = result.value("account");
        const auto account = value.toObject();
        if (!result.contains("account") || (!value.isNull() && (!value.isObject()
                || (account.value("type") != "chatgpt" && account.value("type") != "apiKey")))) {
            fail(tr("Codex returned an invalid account response.")); return;
        }
        if (account.value("type") != "chatgpt") {
            PilonSettings settings;
            const auto enabled = settings.value("assistant/chatgptEnabled"), models = settings.value("assistant/chatgptModels");
            settings.remove("assistant/chatgptEnabled"); settings.remove("assistant/chatgptModels"); settings.sync();
            if (settings.status() != QSettings::NoError) {
                if (enabled.isValid()) settings.setValue("assistant/chatgptEnabled", enabled);
                if (models.isValid()) settings.setValue("assistant/chatgptModels", models);
                settings.sync(); fail(tr("The ChatGPT account state could not be saved.")); return;
            }
        }
        const auto previous = accountIdentity(m_account);
        m_account = account.value("type") == "chatgpt" ? account.toVariantMap() : QVariantMap{};
        const auto identity = accountIdentity(m_account);
        if (previous != identity || identity.isEmpty()) {
            const auto path = accountQuotaPath(m_home);
            const auto saved = NetworkSnapshot::read(path);
            m_rateLimits = {};
            if (!identity.isEmpty() && saved.value("owner") == identity) m_rateLimits = saved.value("buckets").toObject();
            else if (!saved.isEmpty()) QFile::remove(path);
        }
        m_error.clear(); emit stateChanged();
        if (m_account.isEmpty()) { m_rateLimits = {}; m_models.clear(); emit modelsChanged(); emit stateChanged(); }
        else { readModels(); refreshUsage(); }
    });
}
void AssistantConnections::readModels(const QString &cursor, QVariantList accumulated)
{
    QJsonObject params{{"limit", 100}, {"includeHidden", false}};
    if (!cursor.isEmpty()) params["cursor"] = cursor;
    const auto generation = m_accountGeneration;
    request("model/list", params, [this, generation, accumulated](QJsonObject result, QString error) mutable {
        if (generation != m_accountGeneration) return;
        if (!error.isEmpty()) { fail(error); return; }
        for (const auto &value : result.value("data").toArray()) {
            const auto entry = value.toObject(); const QString model = entry.value("model").toString();
            if (model.isEmpty()) continue;
            QVariantList reasoning;
            for (const auto &option : entry.value("supportedReasoningEfforts").toArray()) {
                const QString effort = option.toObject().value("reasoningEffort").toString();
                reasoning.append(QVariantMap{{"id", effort}, {"name", effort}});
            }
            const auto modalities = entry.value("inputModalities").toArray();
            accumulated.append(QVariantMap{{"id", "chatgpt/" + model}, {"name", entry.value("displayName").toString(model)},
                {"provider", "chatgpt"}, {"remoteModel", model}, {"reasoning", reasoning}, {"vision", modalities.contains("image")}, {"audio", modalities.contains("audio")}});
        }
        const auto cursor = result.value("nextCursor").toString();
        if (!cursor.isEmpty()) { readModels(cursor, accumulated); return; }
        m_models = accumulated; PilonSettings().setValue("assistant/chatgptModels", m_models); emit modelsChanged();
    });
}
QVariantList AssistantConnections::models() const
{
    auto result = m_models;
    for (const auto &value : m_servers) {
        auto model = value.toMap(); model["name"] = model.value("name").toString() + " (API)";
        model["provider"] = model.value("protocol");
        model["reasoning"] = QVariantList{QVariantMap{{"id", "none"}, {"name", tr("Default")}}};
        result.append(model);
    }
    return result;
}
bool AssistantConnections::keyring(const QString &action, const QString &id, QByteArray *secret)
{
#ifdef Q_OS_ANDROID
    const auto result = AndroidCredentials::request(action, {{"application", "pilon-api"}, {"connection", id}},
        action == "store" ? *secret : QByteArray{});
    if (action == "lookup") *secret = result.secret;
    return result.ok && (action != "lookup" || result.found);
#endif
    const auto executable = QStandardPaths::findExecutable("secret-tool");
    if (executable.isEmpty()) return false;
    QProcess process;
    QStringList arguments{action};
    if (action == "store") arguments << "--label=Pilon API server";
    arguments << "application" << "pilon-api" << "connection" << id;
    process.start(executable, arguments);
    if (!process.waitForStarted(3000)) return false;
    if (action == "store") { process.write(*secret); process.closeWriteChannel(); }
    if (!process.waitForFinished(5000)) { process.kill(); process.waitForFinished(); return false; }
    const bool noMatches = action == "clear" && process.exitCode() == 1 && process.readAllStandardError().trimmed().isEmpty();
    if (process.exitStatus() != QProcess::NormalExit || (process.exitCode() != 0 && !noMatches)) return false;
    if (action == "lookup") *secret = process.readAllStandardOutput().trimmed();
    return true;
}
QByteArray AssistantConnections::apiKey(const QString &id)
{
    if (!m_keys.contains(id)) { QByteArray key; if (keyring("lookup", id, &key)) m_keys[id] = key; }
    return m_keys.value(id);
}
bool AssistantConnections::saveServer(const QString &existingId, const QString &name, const QString &baseUrl,
                                     const QString &model, const QString &key, const QString &protocol, int context, bool vision, const QVariantMap &modalities)
{
    QUrl url(baseUrl.trimmed());
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "http" && url.scheme() != "https")
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() || name.trimmed().isEmpty() || model.trimmed().isEmpty()
        || (protocol != "responses" && protocol != "chat") || context < 2048 || context > 2097152) {
        fail(tr("Enter a name, HTTP(S) base URL without credentials, model ID, and a context size between 2,048 and 2,097,152.")); return false;
    }
    QString path = url.path(); while (path.endsWith('/')) path.chop(1); url.setPath(path);
    QString id = existingId;
    if (id.isEmpty()) id = "api/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    else if (std::none_of(m_servers.begin(), m_servers.end(), [&](const QVariant &v) { return v.toMap().value("id") == id; })) return false;
    if (!existingId.isEmpty() && key.isEmpty()) {
        for (const auto &value : m_servers) {
            const auto old = value.toMap();
            if (old.value("id") != existingId) continue;
            const QUrl previous(old.value("baseUrl").toString());
            if ((previous.scheme() != url.scheme() || previous.host() != url.host() || previous.port() != url.port())
                && !apiKey(existingId).isEmpty()) {
                fail(tr("Enter a new API key when changing the provider or server address.")); return false;
            }
        }
    }
    if (!key.isEmpty()) {
        auto secret = key.toUtf8();
        if (!keyring("store", id, &secret)) { fail(tr("The system keyring could not save the API key. Unlock your keyring and retry.")); return false; }
        m_keys[id] = secret;
    }
    QVariant hasKey;
    if (!key.isEmpty()) hasKey = true;
    else if (existingId.isEmpty()) hasKey = false;
    else for (const auto &value : m_servers)
        if (value.toMap().value("id") == id) hasKey = value.toMap().value("hasKey");
    QVariantMap entry{{"id", id}, {"name", name.trimmed()}, {"baseUrl", url.toString()}, {"remoteModel", model.trimmed()},
                      {"protocol", protocol}, {"context", context}, {"vision", vision},
                      {"modalityOverrides", modalities}};
    if (hasKey.isValid()) entry["hasKey"] = hasKey;
    for (const auto &kind : {"vision", "audio", "video"})
        if (modalities.contains(kind)) entry[kind] = modalities.value(kind).toInt() == 1;
    bool replaced = false;
    for (auto &value : m_servers) if (value.toMap().value("id") == id) {
        const auto old = value.toMap();
        if (!key.isEmpty()) entry["quotaRevision"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        else if (old.contains("quotaRevision")) entry["quotaRevision"] = old.value("quotaRevision");
        if (serverQuotaPath(old) != serverQuotaPath(entry)) {
            QFile::remove(serverQuotaPath(old));
            m_serverUsage.remove(id);
        }
        value = entry; replaced = true; break;
    }
    if (!replaced) m_servers.append(entry);
    PilonSettings().setValue("assistant/apiServers", m_servers); m_error.clear(); emit modelsChanged(); emit stateChanged(); if (!key.isEmpty()) emit authenticationChanged();
    if (!modalities.isEmpty()) detectModalities(id);
    return true;
}
bool AssistantConnections::removeServer(const QString &id)
{
    bool hasKey = true;
    for (const auto &value : m_servers)
        if (value.toMap().value("id") == id) hasKey = value.toMap().value("hasKey", true).toBool();
    auto remaining = m_servers;
    remaining.removeIf([&](const QVariant &v) { return v.toMap().value("id") == id; });
    if (remaining.size() == m_servers.size()) return false;
    PilonSettings settings;
    settings.setValue("assistant/apiServers", remaining); settings.sync();
    if (settings.status() != PilonSettings::NoError) {
        settings.setValue("assistant/apiServers", m_servers); settings.sync();
        fail(tr("Unable to save the server removal. Check your settings folder permissions.")); return false;
    }
    QByteArray unused;
    if (hasKey && !keyring("clear", id, &unused)) {
        settings.setValue("assistant/apiServers", m_servers); settings.sync();
        fail(tr("Unable to remove this server's API key. Unlock your system keyring and try again.")); return false;
    }
    for (const auto &value : m_servers)
        if (value.toMap().value("id") == id) QFile::remove(serverQuotaPath(value.toMap()));
    m_serverUsage.remove(id);
    m_servers = remaining;
    m_keys.remove(id);
    m_error.clear(); emit modelsChanged(); emit stateChanged(); return true;
}

void AssistantConnections::refreshUsage()
{
    if (m_account.isEmpty() || refreshingUsage() || m_signingIn || m_signingOut) return;
    const auto generation = m_accountGeneration;
    m_usageGeneration = generation;
    emit stateChanged();
    request("account/rateLimits/read", {}, [this, generation](QJsonObject result, QString error) {
        if (generation != m_accountGeneration) return;
        m_usageGeneration = 0;
        if (error.isEmpty()) updateRateLimits(result);
        else emit stateChanged();
    });
    if (!m_client.running() && m_usageGeneration) { m_usageGeneration = 0; emit stateChanged(); }
}
void AssistantConnections::updateRateLimits(const QJsonObject &result)
{
    if (m_account.isEmpty() || m_signingIn || m_signingOut) return;
    if (result.contains("rateLimitsByLimitId") && result.value("rateLimitsByLimitId").isObject())
        m_rateLimits = result.value("rateLimitsByLimitId").toObject();
    else {
        const auto bucket = result.value("rateLimits").toObject();
        if (!bucket.isEmpty()) {
            const auto id = bucket.value("limitId").toString("codex");
            auto merged = m_rateLimits.value(id).toObject();
            for (auto it = bucket.begin(); it != bucket.end(); ++it) if (!it.value().isNull()) merged[it.key()] = it.value();
            m_rateLimits[id] = merged;
        }
    }
    const auto identity = accountIdentity(m_account);
    if (!identity.isEmpty() && !m_home.isEmpty()) {
        QJsonObject buckets;
        for (auto it = m_rateLimits.begin(); it != m_rateLimits.end(); ++it) {
            const auto bucket = it.value().toObject();
            QJsonObject saved;
            if (bucket.value("limitName").isString()) saved["limitName"] = bucket.value("limitName").toString().left(128);
            for (const auto &window : {"primary", "secondary"}) {
                const auto source = bucket.value(window).toObject();
                QJsonObject quota;
                for (const auto &field : {"usedPercent", "windowDurationMins", "resetsAt"})
                    if (source.value(field).isDouble()) quota[field] = source.value(field);
                if (quota.contains("usedPercent")) saved[window] = quota;
            }
            if (!saved.isEmpty()) buckets[it.key().left(128)] = saved;
        }
        NetworkSnapshot::write(accountQuotaPath(m_home), {{"owner", identity}, {"buckets", buckets}});
    }
    emit stateChanged();
}
QVariantList AssistantConnections::usage() const
{
    QVariantList rows;
    for (auto it = m_rateLimits.begin(); it != m_rateLimits.end(); ++it) {
        const auto bucket = it.value().toObject();
        for (const auto &key : {"primary", "secondary"}) {
            const auto window = bucket.value(key).toObject();
            if (!window.value("usedPercent").isDouble()) continue;
            const int minutes = window.value("windowDurationMins").toInt();
            const auto period = minutes % 1440 == 0 && minutes ? tr("%1 days").arg(minutes / 1440)
                : minutes % 60 == 0 && minutes ? tr("%1 hours").arg(minutes / 60) : tr("%1 minutes").arg(minutes);
            const auto remaining = qBound(0.0, 100.0 - window.value("usedPercent").toDouble(), 100.0);
            const auto reset = window.value("resetsAt").toInteger();
            const auto label = bucket.value("limitName").toString(it.key()) + ": " + period;
            rows.append(QVariantMap{{"label", label}, {"remaining", remaining}, {"resetsAt", reset},
                {"text", label + tr(": %1% remaining").arg(remaining, 0, 'f', 0)
                    + (reset ? tr(", resets %1").arg(QDateTime::fromSecsSinceEpoch(reset).toLocalTime().toString("ddd HH:mm")) : QString{})}});
        }
    }
    return rows;
}

void AssistantConnections::updateServerUsage(const QVariantMap &profile, const QVariantMap &limits)
{
    const auto path = serverQuotaPath(profile);
    for (const auto &value : m_servers) {
        const auto server = value.toMap();
        if (server.value("id") != profile.value("id") || path != serverQuotaPath(server)) continue;
        const auto id = server.value("id").toString();
        if (limits.isEmpty()) {
            QFile::remove(path);
            m_serverUsage.remove(id);
        } else {
            m_serverUsage[id] = limits;
            NetworkSnapshot::write(path, QJsonObject::fromVariantMap(limits));
        }
        emit stateChanged();
        break;
    }
}

void AssistantConnections::detectModalities(const QString &id, bool catalogFallback)
{
    QVariantMap server;
    for (const auto &entry : m_servers) if (entry.toMap().value("id") == id) server = entry.toMap();
    if (server.isEmpty()) return;
    const QUrl base(server.value("baseUrl").toString());
    const QHash<QString, QString> providers{{"api.openai.com", "openai"}, {"api.anthropic.com", "anthropic"},
        {"generativelanguage.googleapis.com", "google"}, {"api.mistral.ai", "mistralai"}, {"api.x.ai", "x-ai"}, {"api.deepseek.com", "deepseek"}};
    QUrl url(base); url.setPath(base.path() + "/models");
    const bool directModel = !catalogFallback && base.host() == "api.anthropic.com";
    if (directModel) url = QUrl::fromEncoded(url.toEncoded() + '/' + QUrl::toPercentEncoding(server.value("remoteModel").toString()));
    const auto catalogId = providers.value(base.host()) + '/' + server.value("remoteModel").toString();
    if (catalogFallback) url = QUrl("https://openrouter.ai/api/v1/models");
    QNetworkRequest request(url);
    request.setTransferTimeout(10000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    const auto key = apiKey(id);
    if (!catalogFallback && !key.isEmpty()) request.setRawHeader("Authorization", "Bearer " + key);
    if (directModel) {
        request.setRawHeader("anthropic-version", "2023-06-01");
        if (!key.isEmpty()) request.setRawHeader("x-api-key", key);
    }
    auto *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, server, catalogFallback, catalogId, directModel, canFallback = providers.contains(base.host())] {
        const auto response = QJsonDocument::fromJson(reply->readAll()).object();
        const auto data = directModel && response.value("type") == "model" ? QJsonArray{response} : response.value("data").toArray();
        reply->deleteLater();
        QVariantMap detected;
        for (const auto &value : data) {
            const auto model = value.toObject();
            if (!directModel && model.value("id").toString() != (catalogFallback ? catalogId : server.value("remoteModel").toString())) continue;
            auto inputs = model.value("architecture").toObject().value("input_modalities");
            if (!inputs.isArray()) inputs = model.value("input_modalities");
            if (!inputs.isArray()) inputs = model.value("inputModalities");
            if (inputs.isArray()) {
                const auto list = inputs.toArray();
                detected = {{"vision", list.contains("image")}, {"audio", list.contains("audio")}, {"video", list.contains("video")}};
            } else {
                const auto capabilities = model.value("capabilities").toObject();
                for (const auto &kind : {"vision", "audio", "video"})
                    if (capabilities.value(kind).isBool()) detected[kind] = capabilities.value(kind).toBool();
                for (const auto &kind : {"image", "audio", "video"}) {
                    const auto supported = capabilities.value(QString(kind) + "_input").toObject().value("supported");
                    if (supported.isBool()) detected[QString(kind) == "image" ? "vision" : kind] = supported.toBool();
                }
            }
            break;
        }
        for (auto &value : m_servers) {
            auto entry = value.toMap();
            if (entry.value("id") != id || entry != server) continue; // Ignore deleted/edited profiles and stale replies.
            if (detected.isEmpty() && !catalogFallback && canFallback) { detectModalities(id, true); return; }
            const auto overrides = entry.value("modalityOverrides").toMap();
            for (const auto &kind : {"vision", "audio", "video"})
                entry[kind] = overrides.value(kind, -1).toInt() < 0 ? detected.value(kind, false).toBool() : overrides.value(kind).toInt() == 1;
            entry["detectedModalities"] = detected;
            entry["capabilityStatus"] = detected.isEmpty() ? tr("Capabilities not reported. Set input overrides if needed.") : tr("Input capabilities detected");
            value = entry;
            PilonSettings().setValue("assistant/apiServers", m_servers);
            emit modelsChanged();
            break;
        }
    });
}
