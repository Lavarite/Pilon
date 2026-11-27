#include "calendarformat.h"
#include "calendarmanager.h"
#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QFile>
#include <QFileInfo>
#include <QtConcurrentRun>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include "../../../src/platform/android/androidcredentials.h"
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimeZone>
#include <QUrlQuery>
#include <QUuid>
#include <QXmlStreamReader>
#include <algorithm>
#include <memory>
#include <utility>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QElapsedTimer>
#endif

#ifndef PILON_GOOGLE_CLIENT_ID
#define PILON_GOOGLE_CLIENT_ID ""
#endif
#ifndef PILON_MICROSOFT_CLIENT_ID
#define PILON_MICROSOFT_CLIENT_ID ""
#endif
namespace {
QString uid() { return QUuid::createUuid().toString(QUuid::Id128); }
QString esc(const QString &s) { return QString::fromUtf8(QUrl::toPercentEncoding(s)); }
QByteArray json(const QVariantMap &m) { return QJsonDocument::fromVariant(m).toJson(QJsonDocument::Compact); }
QJsonObject eventPage(const QByteArray &body, const QString &listKey) {
    const auto page = QJsonDocument::fromJson(body).object();
    const auto entries = page.value(listKey);
    if (!entries.isArray()) return {};
    const auto events = entries.toArray();
    for (const auto &event : events)
        if (event.toObject().value("id").toString().isEmpty()) return {};
    return page;
}
bool calendarApiDisabled(const QByteArray &body) {
    const auto error = QJsonDocument::fromJson(body).object().value("error").toObject();
    for (const auto &entry : error.value("errors").toArray())
        if (entry.toObject().value("reason") == "accessNotConfigured")
            return true;
    for (const auto &entry : error.value("details").toArray()) {
        const auto detail = entry.toObject();
        if (detail.value("reason") == "SERVICE_DISABLED" &&
            detail.value("metadata").toObject().value("service") == "calendar-json.googleapis.com")
            return true;
    }
    return false;
}
QString errorText(const QByteArray &body, int status) {
    if (calendarApiDisabled(body))
        return QObject::tr("Enable Google Calendar API in the app's Google Cloud project, then retry. Activation may take a few minutes.");
    auto data = QJsonDocument::fromJson(body).object();
    auto message = data.value("error").isObject() ? data.value("error").toObject().value("message").toString()
                                                  : data.value("error_description").toString();
    if (!message.isEmpty()) return message;
    if (status == 401) return QObject::tr("The calendar account needs to be reconnected.");
    if (status == 403) return QObject::tr("This account does not have access to the calendar.");
    if (status == 0) return QObject::tr("Unable to reach the calendar server. Try syncing again.");
    return QObject::tr("Calendar request failed (%1).").arg(status);
}
QVariantList davResponses(const QByteArray &body, QString *error) {
    QXmlStreamReader xml(body);
    QVariantList result;
    QVariantMap response, properties;
    QStringList path;
    QString status;
    bool inProperties = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name().toString();
            path.append(name);
            if (name == "response") response.clear();
            if (name == "propstat") { properties.clear(); status.clear(); inProperties = true; }
            auto &target = inProperties ? properties : response;
            if (name == "calendar") target["calendar"] = true;
            else if (name == "write" || name == "write-content") target["writable"] = true;
        } else if (xml.isCharacters()) {
            const auto name = path.value(path.size()-1), parent = path.value(path.size()-2);
            const auto text = xml.text().toString();
            auto &target = inProperties ? properties : response;
            if (name == "status" && parent == "propstat") status += text;
            else if (name == "status" && parent == "response") response["status"] = response.value("status").toString() + text;
            else if (name == "href" && (parent == "response" || parent == "current-user-principal" || parent == "calendar-home-set")) {
                const auto key = parent == "response" ? QString("href") : parent;
                target[key] = target.value(key).toString() + text;
            } else if (name == "displayname" || name == "getetag" || name == "calendar-data" || name == "calendar-color")
                target[name] = target.value(name).toString() + text;
        } else if (xml.isEndElement()) {
            if (xml.name() == u"propstat") {
                const auto code = status.simplified().section(' ', 1, 1).toInt();
                if (code >= 200 && code < 300)
                    for (auto it = properties.cbegin(); it != properties.cend(); ++it) response[it.key()] = it.value();
                else if (code == 401 || code == 403) response["errorStatus"] = code;
                inProperties = false;
            } else if (xml.name() == u"response") result.append(response);
            if (!path.isEmpty()) path.removeLast();
        }
    }
    if (xml.hasError() && error) *error = QObject::tr("Invalid calendar server XML.");
    return result;
}
bool appleUrl(const QUrl &url) {
    return url.scheme() == "https" && (url.host() == "caldav.icloud.com" || url.host().endsWith(".icloud.com"));
}
QStringList listStrings(const QVariant &v) {
    QStringList result;
    for (const auto &x : v.toList())
        result << x.toString();
    return result;
}
} // namespace
bool CalendarManager::googleSignInAvailable() const {
#ifdef Q_OS_ANDROID
    return false;
#else
    return true;
#endif
}
QString CalendarManager::googleSignInUnavailable() {
    return tr("Google Calendar sign-in is unavailable on Android.");
}
QString CalendarManager::googleClientId() const { return QString::fromUtf8(PILON_GOOGLE_CLIENT_ID); }
QString CalendarManager::microsoftClientId() const { return QString::fromUtf8(PILON_MICROSOFT_CLIENT_ID); }
std::function<void()> CalendarManager::nativeAuthorization(const QString &provider, const QString &action,
                                                          const QVariantMap &previous, AuthorizationReply done) {
    if (provider == "google" && !googleSignInAvailable()) {
        done({}, googleSignInUnavailable());
        return {};
    }
    if (m_nativeAuthorizer) return m_nativeAuthorizer(provider, action, previous, std::move(done));
#ifdef Q_OS_ANDROID
    auto context = QNativeInterface::QAndroidApplication::context();
    const auto service = QJniObject::fromString(provider), operation = QJniObject::fromString(action),
               client = QJniObject::fromString(provider == "microsoft" ? microsoftClientId() : QString{}),
               metadata = QJniObject::fromString(QString::fromUtf8(json(previous)));
    const jlong id = QJniObject::callStaticMethod<jlong>("org/pilon/PilonCalendarAuth", "begin",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)J",
        context.object(), service.object<jstring>(), operation.object<jstring>(), client.object<jstring>(), metadata.object<jstring>());
    if (!id) { done({}, tr("Calendar authorization could not be started.")); return {}; }
    auto timer = new QTimer(this);
    auto elapsed = std::make_shared<QElapsedTimer>(); elapsed->start();
    connect(timer, &QObject::destroyed, [id] { QJniObject::callStaticMethod<void>("org/pilon/PilonCalendarAuth", "cancel", "(J)V", id); });
    connect(timer, &QTimer::timeout, this, [timer, elapsed, id, action, done = std::move(done)] {
        const auto raw = QJniObject::callStaticObjectMethod("org/pilon/PilonCalendarAuth", "poll", "(J)Ljava/lang/String;", id);
        auto result = QJsonDocument::fromJson(raw.toString().toUtf8()).toVariant().toMap();
        if (result.value("pending").toBool() && elapsed->elapsed() < (action == "connect" ? 180000 : 90000)) return;
        timer->stop(); timer->deleteLater();
        if (result.value("pending").toBool()) done({}, QObject::tr("Calendar authorization timed out. Retry connecting the account."));
        else if (!raw.isValid() || (result.isEmpty() && action != "disconnect")) done({}, QObject::tr("Calendar returned an invalid authorization response."));
        else { const auto error = result.take("error").toString(); done(result, error); }
    });
    timer->start(100);
    return [timer = QPointer<QTimer>(timer), id] {
        QJniObject::callStaticMethod<void>("org/pilon/PilonCalendarAuth", "cancel", "(J)V", id);
        if (timer) { timer->stop(); timer->deleteLater(); }
    };
#else
    done({}, tr("Mobile Calendar authorization is unavailable on this platform."));
    return {};
#endif
}
QString CalendarManager::mobileAuthorizationError(const QString &provider, const QVariantMap &tokens, const QVariantMap &previous) {
    if (tokens.value("native_provider") != provider || tokens.value("access_token").toString().trimmed().isEmpty()
        || tokens.value("access_token").toString().contains(QRegularExpression("[\\r\\n]")) || tokens.contains("refresh_token"))
        return tr("Calendar returned an invalid authorization response.");
    auto scopes = tokens.value("scope").toString().split(' ', Qt::SkipEmptyParts);
    if (provider == "google") {
        if (!scopes.contains("openid") || (!scopes.contains("email") && !scopes.contains("https://www.googleapis.com/auth/userinfo.email"))
            || !scopes.contains("https://www.googleapis.com/auth/calendar.events") || !scopes.contains("https://www.googleapis.com/auth/calendar.calendarlist.readonly"))
            return tr("Google Calendar authorization did not grant the requested Calendar and account permissions.");
        if (!previous.isEmpty() && tokens.value("native_account").toString().compare(previous.value("native_account").toString(), Qt::CaseInsensitive))
            return tr("Calendar returned a different account. Reconnect the selected account.");
    } else if (provider == "microsoft") {
        for (auto &scope : scopes) scope = scope.section('/', -1);
        if (!scopes.contains("User.Read") || (!scopes.contains("Calendars.ReadBasic") && !scopes.contains("Calendars.ReadWrite") && !scopes.contains("Calendars.ReadWrite.Shared")))
            return tr("Microsoft Calendar authorization did not grant Calendar and account permissions.");
        if (tokens.value("native_account").toString().isEmpty() || tokens.value("native_home_authority").toString().isEmpty())
            return tr("Microsoft Calendar returned no account identity.");
        if (!previous.isEmpty() && (tokens.value("native_account") != previous.value("native_account") || tokens.value("native_home_authority") != previous.value("native_home_authority")))
            return tr("Calendar returned a different account. Reconnect the selected account.");
    } else return tr("Unsupported Calendar account provider.");
    return {};
}
void CalendarManager::verifyAccountIdentity(const QString &provider, QVariantMap tokens, AuthorizationReply done) {
    const QUrl url(provider == "google" ? "https://www.googleapis.com/oauth2/v2/userinfo" : "https://graph.microsoft.com/v1.0/me?$select=id,mail,userPrincipalName");
    Reply completed = [this, provider, tokens, done = std::move(done)](int status, QByteArray bytes, QVariantMap) mutable {
        if (status != 200) { done({}, errorText(bytes, status)); return; }
        const auto data = QJsonDocument::fromJson(bytes).toVariant().toMap();
        auto email = data.value("email", data.value("mail")).toString().trimmed();
        if (email.isEmpty()) email = data.value("userPrincipalName").toString().trimmed();
        const auto subject = data.value("id").toString();
        if (email.isEmpty() || subject.isEmpty() || (provider == "google" && !data.value("verified_email").toBool())) {
            done({}, tr("Calendar could not verify the selected account's identity. Retry connecting.")); return;
        }
        if (provider == "google" && !tokens.value("native_account").toString().isEmpty()
            && email.compare(tokens.value("native_account").toString(), Qt::CaseInsensitive)) {
            done({}, tr("Calendar returned a different account. Reconnect the selected account.")); return;
        }
        tokens["native_email"] = email;
        tokens["native_subject"] = subject;
        if (provider == "google") tokens["native_account"] = email;
        done(tokens, {});
    };
    if (m_transport) { m_transport({}, "GET", url, {}, {{"Authorization", "Bearer " + tokens.value("access_token").toString()}}, std::move(completed)); return; }
    QNetworkRequest request(url); request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Authorization", "Bearer " + tokens.value("access_token").toString().toUtf8());
    auto reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, completed = std::move(completed)] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto bytes = reply->readAll(); reply->deleteLater(); completed(status, bytes, {});
    });
}
void CalendarManager::connectMobileAccount(const QString &provider) {
    if (provider == "google" && !googleSignInAvailable()) { fail(googleSignInUnavailable()); return; }
    if (!m_nativeDisconnects.isEmpty()) { fail(tr("Wait for the account to finish disconnecting.")); return; }
    if (provider == "microsoft" && microsoftClientId().isEmpty() && !m_nativeAuthorizer) {
        fail(tr("This build has no Microsoft Android application registration. See the Calendar setup documentation.")); return;
    }
    cancelLogin(); m_loginProvider = provider; m_state = uid(); m_loginTimer.start();
    const auto state = m_state;
    auto cancel = nativeAuthorization(provider, "connect", {}, [this, provider, state](QVariantMap tokens, QString error) {
        if (state != m_state) return;
        if (error.isEmpty()) error = mobileAuthorizationError(provider, tokens);
        if (!error.isEmpty()) { cancelLogin(); fail(error); return; }
        verifyAccountIdentity(provider, tokens, [this, provider, state](QVariantMap verified, QString identityError) {
            if (state != m_state) return;
            cancelLogin();
            if (!identityError.isEmpty()) { fail(identityError); return; }
            acceptVerifiedAccount(provider, verified);
        });
    });
    if (state == m_state) m_cancelNativeLogin = std::move(cancel);
    else if (cancel) cancel();
}
void CalendarManager::acceptVerifiedAccount(const QString &provider, const QVariantMap &tokens) {
    if (!openStorage()) return;
    auto id = uid();
    QVariantMap secret;
    for (auto it = m_accounts.cbegin(); it != m_accounts.cend(); ++it) {
        if (it->value("provider") != provider) continue;
        const auto previous = credentials(it.key());
        const bool sameSubject = previous.value("native_subject") == tokens.value("native_subject") && !previous.value("native_subject").toString().isEmpty();
        const bool legacyEmail = previous.value("native_subject").toString().isEmpty()
            && it->value("email").toString().compare(tokens.value("native_email").toString(), Qt::CaseInsensitive) == 0;
        if (sameSubject || legacyEmail) { id = it.key(); secret = previous; break; }
    }
    auto account = m_accounts.value(id);
    const auto previousAccount = account;
    account["id"] = id; account["provider"] = provider; account["email"] = tokens.value("native_email"); account["status"] = tr("Connected");
    account.remove("pending"); m_accounts[id] = account;
    if (!persist()) {
        if (previousAccount.isEmpty()) m_accounts.remove(id);
        else m_accounts[id] = previousAccount;
        emit changed();
        return;
    }
    if (tokens.contains("native_provider")) secret.clear();
    secret.insert(tokens);
    storeCredentials(id, secret);
    discover(id, [this] { sync(); });
}
void CalendarManager::refreshMobileAccount(const QString &id, const QVariantMap &previous, AuthorizationReply done) {
    m_nativeRefreshes[id].append(std::move(done));
    if (m_nativeRefreshes[id].size() != 1) return;
    const auto provider = m_accounts.value(id).value("provider").toString();
    nativeAuthorization(provider, "refresh", previous, [this, id, provider, previous](QVariantMap tokens, QString error) {
        const auto finished = [this, id, previous](QVariantMap verified, QString failure) {
            if (!m_accounts.contains(id) || m_tokens.value(id) != previous) failure = tr("Calendar authorization changed during renewal. Retry syncing.");
            if (failure.isEmpty() && verified.value("native_subject") != previous.value("native_subject")) failure = tr("Calendar returned a different account. Reconnect the selected account.");
            if (failure.isEmpty()) storeCredentials(id, verified);
            auto callbacks = m_nativeRefreshes.take(id);
            for (auto &callback : callbacks) callback(verified, failure);
        };
        if (error.isEmpty()) error = mobileAuthorizationError(provider, tokens, previous);
        if (!error.isEmpty()) { finished({}, error); return; }
        verifyAccountIdentity(provider, tokens, finished);
    });
}
bool CalendarManager::microsoftCalendarWritable(const QString &account, const QVariantMap &calendar) const {
    if (!calendar.value("canEdit", calendar.value("writable")).toBool()) return false;
    const auto settings = m_accounts.value(account);
    if (!settings.contains("calendarScopes")) return true;
    const auto scopes = settings.value("calendarScopes").toStringList();
    if (scopes.contains("Calendars.ReadWrite.Shared")) return true;
    return scopes.contains("Calendars.ReadWrite") && !calendar.value("owner").toString().isEmpty() &&
        calendar.value("owner").toString().compare(settings.value("email").toString(), Qt::CaseInsensitive) == 0;
}
void CalendarManager::updateMicrosoftPermissions(const QString &id, const QVariantMap &secret) {
    if (m_accounts.value(id).value("provider") != "microsoft" || !secret.contains("scope")) return;
    auto scopes = secret.value("scope").toString().split(' ', Qt::SkipEmptyParts);
    for (auto &scope : scopes) scope = scope.section('/', -1);
    m_accounts[id]["calendarScopes"] = scopes;
    for (auto &calendar : m_calendars) {
        if (calendar.value("account") != id) continue;
        if (!calendar.contains("canEdit")) calendar["canEdit"] = calendar.value("writable");
        calendar["writable"] = microsoftCalendarWritable(id, calendar);
    }
    persist();
}
bool CalendarManager::keyring(const QString &action, const QString &id, QByteArray *secret) {
#ifdef Q_OS_ANDROID
    const auto result = AndroidCredentials::request(action, {{"application", "pilon-calendar"}, {"account", id}},
        action == "store" ? *secret : QByteArray{});
    if (action == "lookup") *secret = result.secret;
    return result.ok && (action != "lookup" || result.found);
#endif
    auto executable = QStandardPaths::findExecutable("secret-tool");
    if (executable.isEmpty())
        return false;
    QProcess process;
    QStringList args{action};
    if (action == "store")
        args << "--label=Pilon Calendar";
    args << "application" << "pilon-calendar" << "account" << id;
    process.start(executable, args);
    if (!process.waitForStarted(3000))
        return false;
    if (action == "store") {
        process.write(*secret);
        process.closeWriteChannel();
    }
    if (!process.waitForFinished(action == "store" ? 120000 : 5000)) {
        process.kill();
        process.waitForFinished();
        return false;
    }
    const bool noMatches = action == "clear" && process.exitCode() == 1 && process.readAllStandardError().trimmed().isEmpty();
    if (process.exitStatus() != QProcess::NormalExit || (process.exitCode() != 0 && !noMatches))
        return false;
    if (action == "lookup")
        *secret = process.readAllStandardOutput().trimmed();
    return true;
}
QVariantMap CalendarManager::credentials(const QString &id) {
    if (m_tokens.contains(id))
        return m_tokens[id];
    QByteArray bytes;
    if (keyring("lookup", id, &bytes)) {
        m_tokens[id] = QJsonDocument::fromJson(bytes).toVariant().toMap();
        updateMicrosoftPermissions(id, m_tokens[id]);
    }
    return m_tokens.value(id);
}
void CalendarManager::loadCredentials(const QString &id, std::function<void()> done) {
    if (m_tokens.contains(id)) { done(); return; }
    auto watcher = new QFutureWatcher<QVariantMap>(this);
    connect(watcher, &QFutureWatcher<QVariantMap>::finished, this, [this, watcher, id, done] {
        // A reconnect may have supplied newer credentials while the lookup was running.
        const auto secret = watcher->result();
        if ((m_accounts.contains(id) || id == "calendar-sharing") && !m_tokens.contains(id) && !secret.isEmpty()) {
            m_tokens[id] = secret;
            if (m_accounts.contains(id)) {
                updateMicrosoftPermissions(id, secret);
                if (m_accounts[id].remove("credentialWarning")) persist();
            }
        }
        watcher->deleteLater();
        done();
    });
    watcher->setFuture(QtConcurrent::run([id] {
        QByteArray bytes;
        keyring("lookup", id, &bytes);
        return QJsonDocument::fromJson(bytes).toVariant().toMap();
    }));
}
void CalendarManager::storeCredentials(const QString &id, const QVariantMap &secret) {
    m_tokens[id] = secret;
    updateMicrosoftPermissions(id, secret);
    if (m_credentialWrites.contains(id)) return;
    m_credentialWrites.insert(id);
    auto bytes = json(secret);
    const auto finished = [this, id, secret](bool ok) {
        m_credentialWrites.remove(id);
        if (!m_accounts.contains(id)) {
            // A sign-out or failed connection can finish before the wallet prompt.
            (void)QtConcurrent::run([id] { QByteArray unused; keyring("clear", id, &unused); });
            return;
        }
        if (m_tokens.value(id) != secret) {
            storeCredentials(id, m_tokens.value(id));
            return;
        }
        if (!ok) {
#ifdef Q_OS_ANDROID
            m_accounts[id]["credentialWarning"] = tr("Pilon could not save this account in Android's secure storage. Unlock the device and retry connecting.");
#else
            m_accounts[id]["credentialWarning"] = tr("Pilon could not save this account in the system keyring. "
                                                 "Set up or unlock your keyring, then retry.");
#endif
            fail(m_accounts[id].value("credentialWarning").toString());
        } else {
            if (m_error == m_accounts[id].value("credentialWarning").toString()) m_error.clear();
            m_accounts[id].remove("credentialWarning");
        }
        persist();
    };
    auto watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [watcher, finished] {
        finished(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([id, bytes]() mutable { return keyring("store", id, &bytes); }));
}
void CalendarManager::request(const QString &account, const QByteArray &method, const QUrl &url, const QByteArray &body,
                              const QVariantMap &headers, Reply done, bool retried, int redirects) {
    if (url.scheme() != "https" || !url.userInfo().isEmpty()) {
        done(0, {}, {});
        return;
    }
    const auto provider = m_accounts.value(account).value("provider").toString();
    if (provider == "google" && !googleSignInAvailable()) {
        done(403, json({{"error", QVariantMap{{"message", googleSignInUnavailable()}}}}), {});
        return;
    }
    const auto scopes = m_accounts.value(account).value("calendarScopes").toStringList();
    if (provider == "microsoft" && m_accounts.value(account).contains("calendarScopes") &&
        !scopes.contains("Calendars.ReadWrite") && !scopes.contains("Calendars.ReadWrite.Shared") &&
        (method == "POST" || method == "PATCH" || method == "PUT" || method == "DELETE")) {
        done(403, json({{"error", QVariantMap{{"message", tr("Microsoft calendar access is read-only.")}}}}), {});
        return;
    }
    const QPointer<CalendarManager> self(this);
    const auto completed = std::make_shared<bool>(false);
    done = [self, completed, callback = std::move(done)](int status, QByteArray bytes, QVariantMap metadata) {
        if (!self || *completed) return;
        *completed = true;
        callback(status, std::move(bytes), std::move(metadata));
    };
    if (m_transport) {
        m_transport(account, method, url, body, headers, done);
        return;
    }
    if (!m_tokens.contains(account) && m_accounts.contains(account)) {
        loadCredentials(account, [=, this] {
            if (!m_accounts.contains(account)) { done(0, {}, {}); return; }
            if (!m_tokens.contains(account)) {
                done(0, json({{"error", QVariantMap{{"message", tr("Calendar account credentials are unavailable.")}}}}), {});
                return;
            }
            this->request(account, method, url, body, headers, done, retried, redirects);
        });
        return;
    }
    QNetworkRequest request(url);
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto secret = credentials(account);
    if (provider == "icloud") {
        if (!appleUrl(url)) {
            done(0, {}, {});
            return;
        }
        request.setRawHeader(
            "Authorization",
            "Basic " +
                (secret.value("email").toString() + ":" + secret.value("password").toString()).toUtf8().toBase64());
    } else if (provider == "google" || provider == "microsoft") {
        const auto expected = provider == "google" ? "www.googleapis.com" : "graph.microsoft.com";
        if (url.host() != expected) {
            done(0, {}, {});
            return;
        }
        request.setRawHeader("Authorization", "Bearer " + secret.value("access_token").toString().toUtf8());
    }
    for (auto it = headers.begin(); it != headers.end(); ++it)
        request.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
    auto reply = m_network.sendCustomRequest(request, method, body);
    connect(reply, &QNetworkReply::finished, this, [=, this] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        auto bytes = reply->readAll();
        if (!status && reply->error() != QNetworkReply::NoError)
            bytes = json({{"error", QVariantMap{{"message", reply->errorString()}}}});
        QVariantMap responseHeaders;
        for (const auto &h : reply->rawHeaderPairs())
            responseHeaders[QString::fromUtf8(h.first).toLower()] = QString::fromUtf8(h.second);
        auto redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        reply->deleteLater();
        if (status >= 300 && status < 400 && !redirect.isEmpty()) {
            auto next = url.resolved(redirect);
            bool trusted =
                provider == "icloud" ? appleUrl(next) : next.scheme() == "https" && next.host() == url.host();
            if (!trusted || redirects >= 5) {
                done(0, {}, {});
                return;
            }
            this->request(account, method, next, body, headers, done, retried, redirects + 1);
            return;
        }
        if (!m_accounts.contains(account)) {
            done(0, {}, {});
            return;
        }
        if (status == 401 && !retried && (provider == "google" || provider == "microsoft") &&
            secret.value("native_provider") == provider) {
            refreshMobileAccount(account, secret, [=, this](QVariantMap, QString error) {
                if (!m_accounts.contains(account)) { done(0, {}, {}); return; }
                if (!error.isEmpty()) { m_accounts[account]["status"] = tr("Reconnect required"); persist(); done(401, bytes, responseHeaders); return; }
                this->request(account, method, url, body, headers, done, true);
            });
            return;
        }
        if (status == 401 && !retried && (provider == "google" || provider == "microsoft") &&
            !secret.value("refresh_token").toString().isEmpty()) {
            tokenRequest(provider, {{"grant_type", "refresh_token"}, {"refresh_token", secret.value("refresh_token")}},
                         [=, this](QVariantMap tokens, QString error) {
                             if (!error.isEmpty()) {
                                 m_accounts[account]["status"] = tr("Reconnect required");
                                 persist();
                                 done(401, bytes, responseHeaders);
                                 return;
                             }
                             auto combined = secret;
                             for (auto i = tokens.begin(); i != tokens.end(); ++i)
                                 combined[i.key()] = i.value();
                             storeCredentials(account, combined);
                             this->request(account, method, url, body, headers, done, true);
                         });
            return;
        }
        if (status == 429 || status >= 500) {
            int delay = responseHeaders.value("retry-after").toInt();
            if (delay <= 0)
                delay = std::min(3600, 60 * (1 << std::min(6, m_accounts[account].value("failures").toInt())));
            m_accounts[account]["retryAt"] = QDateTime::currentDateTimeUtc().addSecs(delay).toString(Qt::ISODate);
            m_accounts[account]["failures"] = m_accounts[account].value("failures").toInt() + 1;
        } else if (status >= 200 && status < 300) {
            m_accounts[account].remove("retryAt");
            m_accounts[account].remove("failures");
        }
        done(status, bytes, responseHeaders);
    });
}
void CalendarManager::tokenRequest(const QString &provider, const QVariantMap &fields,
                                   std::function<void(QVariantMap, QString)> done) {
    QUrlQuery form;
    for (auto i = fields.begin(); i != fields.end(); ++i)
        form.addQueryItem(i.key(), i.value().toString());
    form.addQueryItem("client_id", provider == "google" ? googleClientId() : microsoftClientId());
    const auto clientSecret = qEnvironmentVariable("PILON_GOOGLE_CLIENT_SECRET");
    if (provider == "google" && !clientSecret.isEmpty())
        form.addQueryItem("client_secret", clientSecret);
    const QUrl url(provider == "google" ? "https://oauth2.googleapis.com/token"
                                        : "https://login.microsoftonline.com/common/oauth2/v2.0/token");
    const auto body = form.toString(QUrl::FullyEncoded).toUtf8();
    const auto completed = [done = std::move(done)](int status, const QByteArray &bytes) {
        auto result = QJsonDocument::fromJson(bytes).toVariant().toMap();
        done(result, status == 200 && !result.value("access_token").toString().isEmpty() ? QString{}
                                                                                         : errorText(bytes, status));
    };
    if (m_transport) {
        m_transport({}, "POST", url, body, {{"Content-Type", "application/x-www-form-urlencoded"}},
                    [completed](int status, QByteArray bytes, QVariantMap) { completed(status, bytes); });
        return;
    }
    QNetworkRequest request(url);
    request.setTransferTimeout(30000);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    auto reply = m_network.post(request, body);
    connect(reply, &QNetworkReply::finished, this, [reply, completed] {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto bytes = reply->readAll();
        reply->deleteLater();
        completed(status, bytes);
    });
}
void CalendarManager::connectAccount(const QString &provider, const QString &email, const QString &password) {
    if (provider == "icloud") {
        if (!email.contains('@') || password.trimmed().isEmpty()) {
            fail(tr("Enter your Apple Account email and an app-specific password."));
            return;
        }
        if (!openStorage()) return;
        auto id = uid();
        for (auto it = m_accounts.cbegin(); it != m_accounts.cend(); ++it)
            if (it->value("provider") == provider &&
                it->value("email").toString().compare(email.trimmed(), Qt::CaseInsensitive) == 0) {
                id = it.key();
                break;
            }
        const bool pending = !m_accounts.contains(id);
        m_accounts[id] = {
            {"id", id}, {"provider", provider}, {"email", email.trimmed()}, {"status", tr("Connecting…")}, {"pending", pending}};
        storeCredentials(id, {{"email", email.trimmed()}, {"password", password.trimmed()}});
        persist();
        discover(id, [this, id] {
            if (m_accounts.value(id).value("pending").toBool()) {
                const auto message = m_accounts[id].value("status").toString();
                QByteArray unused;
                keyring("clear", id, &unused);
                m_tokens.remove(id);
                m_accounts.remove(id);
                persist();
                fail(message);
            } else {
                sync();
            }
        });
        return;
    }
    if (provider != "google" && provider != "microsoft")
        return;
#ifdef Q_OS_ANDROID
    connectMobileAccount(provider);
    return;
#endif
    auto client = provider == "google" ? googleClientId() : microsoftClientId();
    if (client.isEmpty()) {
        fail(tr("This build has no %1 application registration. See the calendar "
                "setup documentation.")
                 .arg(provider));
        return;
    }
    cancelLogin();
    m_loginProvider = provider;
    m_state = uid() + uid();
    m_verifier = uid() + uid();
    if (!m_callback.listen(QHostAddress::LocalHost, 0)) {
        fail(tr("Cannot open the local sign-in callback."));
        return;
    }
    m_redirect =
        QString("http://%1:%2").arg(provider == "google" ? "127.0.0.1" : "localhost").arg(m_callback.serverPort());
    connect(&m_callback, &QTcpServer::newConnection, this, [this] {
        while (auto socket = m_callback.nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(10000, socket, &QTcpSocket::disconnectFromHost);
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                *buffer += socket->readAll();
                if (buffer->size() > 16384) {
                    socket->disconnectFromHost();
                    return;
                }
                if (!buffer->contains("\r\n\r\n"))
                    return;
                auto parts = buffer->split(' ');
                auto target = parts.value(1);
                QUrlQuery query(QUrl("http://localhost" + QString::fromUtf8(target)));
                bool valid = parts.value(0) == "GET" && query.queryItemValue("state") == m_state && !m_state.isEmpty();
                const QByteArray message = valid ? "You can return to Pilon." : "Invalid sign-in response.";
                socket->write("HTTP/1.1 " + QByteArray(valid ? "200 OK" : "400 Bad Request") +
                              "\r\nContent-Type: text/plain\r\nConnection: "
                              "close\r\nContent-Length: " +
                              QByteArray::number(message.size()) + "\r\n\r\n" + message);
                socket->disconnectFromHost();
                if (!valid)
                    return;
                auto code = query.queryItemValue("code");
                m_callback.close();
                m_loginTimer.stop();
                if (code.isEmpty()) {
                    cancelLogin();
                    fail(tr("Calendar sign-in was cancelled or denied."));
                } else
                    completeLogin(code);
            });
        }
    });
    QUrl url(provider == "google" ? "https://accounts.google.com/o/oauth2/v2/auth"
                                  : "https://login.microsoftonline.com/common/oauth2/v2.0/authorize");
    QUrlQuery q;
    q.addQueryItem("client_id", client);
    q.addQueryItem("redirect_uri", m_redirect);
    q.addQueryItem("response_type", "code");
    q.addQueryItem("state", m_state);
    q.addQueryItem("code_challenge_method", "S256");
    q.addQueryItem("code_challenge",
                   QString::fromUtf8(QCryptographicHash::hash(m_verifier.toUtf8(), QCryptographicHash::Sha256)
                                         .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
    q.addQueryItem("scope", provider == "google" ? "openid email https://www.googleapis.com/auth/calendar.events "
                                                   "https://www.googleapis.com/auth/calendar.calendarlist.readonly"
                                                 : "openid profile offline_access User.Read Calendars.ReadBasic Calendars.ReadWrite "
                                                   "Calendars.ReadWrite.Shared");
    if (provider == "google") {
        q.addQueryItem("access_type", "offline");
        q.addQueryItem("prompt", "consent select_account");
    } else
        q.addQueryItem("prompt", "select_account");
    url.setQuery(q);
    m_loginTimer.start();
    if (!QDesktopServices::openUrl(url)) {
        cancelLogin();
        fail(tr("Could not open your browser for sign-in."));
    }
}
void CalendarManager::cancelLogin() {
    m_state.clear();
    if (auto cancel = std::exchange(m_cancelNativeLogin, {})) cancel();
    m_callback.close();
    disconnect(&m_callback, nullptr, this, nullptr);
    m_loginTimer.stop();
    m_loginProvider.clear();
    m_verifier.clear();
}
void CalendarManager::completeLogin(const QString &code) {
    auto provider = m_loginProvider;
    auto state = m_state;
    tokenRequest(
        provider,
        {{"grant_type", "authorization_code"},
         {"code", code},
         {"redirect_uri", m_redirect},
         {"code_verifier", m_verifier}},
        [this, provider, state](QVariantMap tokens, QString error) {
            if (state != m_state)
                return;
            if (!error.isEmpty()) { cancelLogin(); fail(error); return; }
            verifyAccountIdentity(provider, tokens, [this, provider, state](QVariantMap verified, QString identityError) {
                if (state != m_state) return;
                cancelLogin();
                if (!identityError.isEmpty()) { fail(identityError); return; }
                acceptVerifiedAccount(provider, verified);
            });
        });
}
void CalendarManager::removeCalendar(const QString &id) {
    for (auto e = m_events.begin(); e != m_events.end();)
        if (e->value("source") == id && !e->value("local").toBool())
            e = m_events.erase(e);
        else {
            if (e->value("organizerCalendar") == id) (*e)["organizerCalendar"] = "";
            ++e;
        }
    for (auto l = m_links.begin(); l != m_links.end();)
        if (l->value("calendar") == id) {
            auto eventId = l->value("event").toString();
            auto dest = listStrings(m_events.value(eventId).value("destinations"));
            dest.removeAll(id);
            if (m_events.contains(eventId)) m_events[eventId]["destinations"] = dest;
            m_conflicts.remove(l.key());
            l = m_links.erase(l);
        } else
            ++l;
    m_instances.remove(id);
    m_calendars.remove(id);
}
bool CalendarManager::disconnectAccount(const QString &id) {
    if (!m_accounts.contains(id)) return true;
    if (m_syncing || m_credentialWrites.contains(id) || m_nativeRefreshes.contains(id) || !m_loginProvider.isEmpty()) {
        fail(tr("Wait for the current sync or sign-in before disconnecting."));
        return false;
    }
    QVariantMap secret;
    const auto provider = m_accounts.value(id).value("provider").toString();
    if ((provider == "google" && googleSignInAvailable()) || provider == "microsoft") {
#ifdef Q_OS_ANDROID
        secret = credentials(id);
#else
        if (m_nativeAuthorizer) secret = credentials(id);
#endif
    }
    if (!secret.value("native_provider").toString().isEmpty() && !m_nativeDisconnectReady.contains(id)) {
        if (m_nativeDisconnects.contains(id)) return false;
        m_nativeDisconnects.insert(id);
        fail(tr("Disconnecting the Calendar account. Wait for it to finish before disabling Calendar."));
        nativeAuthorization(m_accounts.value(id).value("provider").toString(), "disconnect", secret, [this, id, secret](QVariantMap, QString error) {
            m_nativeDisconnects.remove(id);
            if (!m_accounts.contains(id) || m_tokens.value(id) != secret) { fail(tr("The Calendar account changed during disconnect. Retry disconnecting.")); return; }
            if (!error.isEmpty()) { fail(error); return; }
            m_nativeDisconnectReady.insert(id);
            if (disconnectAccount(id)) { m_error.clear(); emit changed(); }
        });
        return false;
    }
    const auto accounts = m_accounts, calendars = m_calendars, events = m_events,
               links = m_links, conflicts = m_conflicts, instances = m_instances;
    const auto restore = [&] {
        m_accounts = accounts; m_calendars = calendars; m_events = events;
        m_links = links; m_conflicts = conflicts; m_instances = instances;
    };
    for (const auto &source : m_calendars.keys())
        if (m_calendars.value(source).value("account") == id) removeCalendar(source);
    m_accounts.remove(id);
    if (!persist()) { restore(); emit changed(); return false; }
    if (accounts.value(id).value("provider") != "ics") {
        QByteArray secret;
        if (!keyring("clear", id, &secret)) {
            restore(); persist();
#ifdef Q_OS_ANDROID
            fail(tr("Unable to remove this account from Android's secure storage. Unlock the device and retry disconnecting."));
#else
            fail(tr("Unable to remove this account from the system keyring. Unlock it and try again."));
#endif
            return false;
        }
    }
    m_tokens.remove(id);
    m_nativeDisconnectReady.remove(id);
    return true;
}
bool CalendarManager::accountOperationsBusy() const {
    return !m_credentialWrites.isEmpty() || !m_nativeDisconnects.isEmpty() || std::any_of(m_accounts.cbegin(), m_accounts.cend(), [](const auto &account) {
        return account.value("pending").toBool();
    });
}
void CalendarManager::addFeed(const QString &address, const QString &name) {
    QUrl url(address.trimmed());
    if (url.scheme() == "webcal")
        url.setScheme("https");
    if (url.scheme() != "https" || url.host().isEmpty() || !url.userInfo().isEmpty()) {
        fail(tr("Enter an HTTPS or webcal calendar URL without embedded login "
                "credentials."));
        return;
    }
    auto id = uid();
    m_accounts[id] = {
        {"id", id}, {"provider", "feed"}, {"email", name.trimmed().isEmpty() ? url.host() : name.trimmed()}};
    storeCredentials(id, {{"url", url.toString()}});
    m_calendars[id] = {{"id", id},           {"account", id},
                       {"provider", "feed"}, {"name", m_accounts[id].value("email")},
                       {"enabled", true},    {"writable", false}};
    persist();
    sync();
}
bool CalendarManager::addIcsFile(const QUrl &url) {
    if (!m_active) { fail(tr("Enable Calendar to import events.")); return false; }
    const QFileInfo info(url.toLocalFile());
    QFile file(info.filePath());
    if (!url.isLocalFile() || !info.isFile() || info.suffix().compare("ics", Qt::CaseInsensitive)
        || !file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) {
        fail(tr("Choose a readable iCalendar (.ics) file up to 8 MB."));
        return false;
    }
    QString error;
    const auto events = CalendarFormat::readIcs(file.readAll(), &error);
    QVariantList rows;
    for (const auto &value : events) {
        auto event = value.toMap();
        if (event.value("title").toString().trimmed().isEmpty()) event["title"] = tr("Untitled event");
        error = event.value("cancelled").toBool() ? QString{} : CalendarFormat::validate(event);
        if (error.isEmpty() && event.value("uid").toString().isEmpty())
            error = tr("An iCalendar event is missing its UID.");
        if (!error.isEmpty()) break;
        rows.append(QVariantMap{{"remoteId", event.value("uid")}, {"event", event}});
    }
    if (!error.isEmpty()) { fail(error); return false; }
    std::stable_sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("event").toMap().value("recurrenceId").toString().isEmpty()
            && !b.toMap().value("event").toMap().value("recurrenceId").toString().isEmpty();
    });
    const auto id = uid();
    m_accounts[id] = {{"id", id}, {"provider", "ics"}, {"email", info.fileName()}};
    m_calendars[id] = {{"id", id}, {"account", id}, {"provider", "ics"},
        {"name", info.completeBaseName()}, {"enabled", true}, {"writable", false}};
    ingest(id, rows, true);
    if (persist()) return true;
    for (auto it = m_events.begin(); it != m_events.end();)
        if (it->value("source") == id) it = m_events.erase(it); else ++it;
    for (auto it = m_links.begin(); it != m_links.end();)
        if (it->value("calendar") == id) it = m_links.erase(it); else ++it;
    m_calendars.remove(id); m_accounts.remove(id);
    return false;
}
void CalendarManager::discover(const QString &account, std::function<void()> done) {
    if (!m_accounts.contains(account)) {
        done();
        return;
    }
    auto provider = m_accounts[account].value("provider").toString();
    if (provider == "feed" || provider == "ics") {
        done();
        return;
    }
    auto discovered = std::make_shared<Records>();
    auto accept = [this, account, provider, discovered](const QString &remote, const QString &name, bool writable,
                                            const QString &url = QString{}, const QString &owner = QString{}) {
        auto id = account + ":" + remote;
        QVariantMap c;
        c["id"] = id;
        c["account"] = account;
        c["provider"] = provider;
        c["remoteId"] = remote;
        c["name"] = name;
        c["writable"] = writable;
        if (provider == "microsoft") {
            c["canEdit"] = writable;
            c["owner"] = owner;
            c["writable"] = microsoftCalendarWritable(account, c);
        }
        if (!url.isEmpty())
            c["url"] = url;
        (*discovered)[id] = c;
    };
    // Reconcile only a complete, successful listing; an interrupted discovery
    // must not erase cached calendars or overwrite their enabled state.
    auto reconcile = [this, account, discovered] {
        for (auto it = discovered->cbegin(); it != discovered->cend(); ++it) {
            auto c = m_calendars.value(it.key());
            for (auto property = it->cbegin(); property != it->cend(); ++property)
                c[property.key()] = property.value();
            if (!c.contains("enabled")) c["enabled"] = false;
            m_calendars[it.key()] = c;
        }
        for (const auto &id : m_calendars.keys())
            if (m_calendars.value(id).value("account") == account && !discovered->contains(id))
                removeCalendar(id);
    };
    if (provider == "icloud") {
        const QByteArray principal = "<?xml version=\"1.0\"?><d:propfind "
                                     "xmlns:d=\"DAV:\"><d:prop><d:current-user-"
                                     "principal/></d:prop></d:propfind>";
        request(account, "PROPFIND", QUrl("https://caldav.icloud.com/"), principal,
                {{"Depth", "0"}, {"Content-Type", "application/xml"}},
                [=, this](int status, QByteArray bytes, QVariantMap) {
                    QString error;
                    auto records = davResponses(bytes, &error);
                    if (status != 207 || !error.isEmpty() || records.isEmpty()) {
                        m_accounts[account]["status"] = tr("Unable to connect to iCloud; check the app-specific "
                                                           "password.");
                        persist();
                        done();
                        return;
                    }
                    m_accounts[account].remove("pending");
                    QUrl principalUrl =
                        QUrl("https://caldav.icloud.com/")
                            .resolved(QUrl(records.first().toMap().value("current-user-principal").toString()));
                    request(account, "PROPFIND", principalUrl,
                            "<d:propfind xmlns:d=\"DAV:\" "
                            "xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop><c:calendar-"
                            "home-set/></d:prop></d:propfind>",
                            {{"Depth", "0"}, {"Content-Type", "application/xml"}},
                            [=, this](int s, QByteArray b, QVariantMap) {
                                QString error;
                                auto rows = davResponses(b, &error);
                                if (s != 207 || rows.isEmpty() || !error.isEmpty()) {
                                    fail(tr("iCloud calendar discovery failed."));
                                    done();
                                    return;
                                }
                                auto home = principalUrl.resolved(
                                    QUrl(rows.first().toMap().value("calendar-home-set").toString()));
                                request(account, "PROPFIND", home,
                                        "<d:propfind "
                                        "xmlns:d=\"DAV:\"><d:prop><d:displayname/><d:resourcetype/"
                                        "><d:current-user-privilege-set/></d:prop></d:propfind>",
                                        {{"Depth", "1"}, {"Content-Type", "application/xml"}},
                                        [=, this](int s, QByteArray b, QVariantMap) {
                                            QString error;
                                            QXmlStreamReader listing(b);
                                            if (!listing.readNextStartElement() || listing.name() != u"multistatus"
                                                || listing.namespaceUri() != u"DAV:")
                                                error = tr("Invalid calendar server XML.");
                                            auto rows = davResponses(b, &error);
                                            for (const auto &v : rows) {
                                                const auto r = v.toMap();
                                                const auto status = r.value("errorStatus", r.value("status").toString().simplified().section(' ', 1, 1).toInt()).toInt();
                                                if (status >= 400 && status != 404 && status != 410) error = errorText({}, status);
                                                if (r.value("calendar").toBool() && r.value("href").toString().trimmed().isEmpty())
                                                    error = tr("Invalid calendar server XML.");
                                            }
                                            if (s == 207 && error.isEmpty()) {
                                                for (const auto &v : rows) {
                                                    auto r = v.toMap();
                                                    if (r.value("calendar").toBool()) {
                                                        auto url = home.resolved(QUrl(r.value("href").toString()));
                                                        if (appleUrl(url))
                                                            accept(url.toString(), r.value("displayname").toString(),
                                                                   r.value("writable").toBool(), url.toString());
                                                    }
                                                }
                                                reconcile();
                                                m_accounts[account]["status"] = tr("Connected");
                                            } else
                                                m_accounts[account]["status"] = error.isEmpty() ? errorText(b, s) : error;
                                            persist();
                                            done();
                                        });
                            });
                });
        return;
    }
    auto page = std::make_shared<std::function<void(QUrl)>>();
    std::weak_ptr<std::function<void(QUrl)>> weak = page;
    *page = [=, this](QUrl url) {
        request(account, "GET", url, {}, {}, [=, this, keep = weak.lock()](int status, QByteArray bytes, QVariantMap) {
            if (status != 200) {
                m_accounts[account]["status"] = errorText(bytes, status);
                m_accounts[account].remove("setupUrl");
                if (provider == "google" && calendarApiDisabled(bytes)) {
                    QString project = googleClientId().section('-', 0, 0);
                    const auto details = QJsonDocument::fromJson(bytes).object().value("error").toObject().value("details").toArray();
                    for (const auto &entry : details) {
                        const auto consumer = entry.toObject().value("metadata").toObject().value("consumer").toString();
                        if (consumer.startsWith("projects/"))
                            project = consumer.mid(9);
                    }
                    const bool numeric = !project.isEmpty() && std::all_of(project.cbegin(), project.cend(), [](QChar c) { return c >= u'0' && c <= u'9'; });
                    m_accounts[account]["setupUrl"] = QStringLiteral("https://console.cloud.google.com/apis/library/calendar-json.googleapis.com") +
                        (numeric ? "?project=" + project : QString{});
                }
                persist();
                done();
                return;
            }
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(bytes, &parseError);
            const auto listKey = provider == "google" ? "items" : "value";
            if (parseError.error != QJsonParseError::NoError || !document.isObject()
                || !document.object().value(listKey).isArray()) {
                m_accounts[account]["status"] = tr("Invalid calendar server response.");
                persist(); done(); return;
            }
            auto data = document.toVariant().toMap();
            auto rows = data.value(listKey).toList();
            for (const auto &v : rows) {
                auto r = v.toMap();
                if (r.value("deleted").toBool()) continue;
                if (r.value("id").toString().isEmpty()) {
                    m_accounts[account]["status"] = tr("Invalid calendar server response.");
                    persist(); done(); return;
                }
                auto name = r.value(provider == "google" ? "summary" : "name").toString();
                if (provider == "google" && !r.value("summaryOverride").toString().isEmpty())
                    name = r.value("summaryOverride").toString();
                accept(r.value("id").toString(), name,
                       provider == "google" ? QStringList{"owner", "writer"}.contains(r.value("accessRole").toString())
                                            : r.value("canEdit").toBool(), {},
                       r.value("owner").toMap().value("address").toString());
            }
            auto next = data.value(provider == "google" ? "nextPageToken" : "@odata.nextLink").toString();
            if (!next.isEmpty() && keep) {
                if (provider == "google") {
                    QUrl u("https://www.googleapis.com/calendar/v3/users/me/"
                           "calendarList");
                    QUrlQuery q;
                    q.addQueryItem("pageToken", next);
                    q.addQueryItem("showHidden", "true");
                    u.setQuery(q);
                    (*keep)(u);
                } else
                    (*keep)(QUrl(next));
                return;
            }
            reconcile();
            m_accounts[account]["status"] = tr("Connected");
            m_accounts[account].remove("setupUrl");
            persist();
            done();
        });
    };
    (*page)(QUrl(provider == "google" ? "https://www.googleapis.com/calendar/v3/users/me/calendarList?showHidden=true"
                                      : "https://graph.microsoft.com/v1.0/me/calendars?$top=100"));
}
void CalendarManager::retrySync() {
    for (auto it = m_accounts.cbegin(); it != m_accounts.cend(); ++it)
        if (!it->value("credentialWarning").toString().isEmpty() && m_tokens.contains(it.key()))
            storeCredentials(it.key(), m_tokens.value(it.key()));
    sync();
}
void CalendarManager::sync() {
    if (!m_active) return;
    if (m_interactionActive || m_editSyncTimer.isActive() || m_syncing) {
        m_resync = true;
        return;
    }
    m_resync = false;
    m_syncing = true;
    touch();
    m_syncSources.clear();
    auto ids = std::make_shared<QStringList>(m_accounts.keys());
    auto step = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = step;
    *step = [this, ids, weak] {
        if (m_interactionActive || m_editSyncTimer.isActive()) {
            m_resync = true;
            syncNext();
            return;
        }
        if (ids->isEmpty()) {
            for (auto it = m_calendars.cbegin(); it != m_calendars.cend(); ++it)
                if (it->value("enabled").toBool() && it.key() != "pilon")
                    m_syncSources << it.key();
            syncNext();
            return;
        }
        auto id = ids->takeFirst();
        auto next = weak.lock();
        if (QDateTime::fromString(m_accounts[id].value("retryAt").toString(), Qt::ISODate) >
            QDateTime::currentDateTimeUtc()) {
            if (next)
                (*next)();
            return;
        }
        discover(id, [next] {
            if (next)
                (*next)();
        });
    };
    (*step)();
}
void CalendarManager::syncNext() {
    if (!m_active) { m_syncing = false; return; }
    if (m_interactionActive || m_editSyncTimer.isActive()) {
        m_resync = true;
        m_syncSources.clear();
    }
    if (m_syncSources.isEmpty()) {
        m_syncing = false;
        persist();
        if (m_resync && !m_interactionActive && !m_editSyncTimer.isActive()) {
            QTimer::singleShot(0, this, &CalendarManager::sync);
        }
        return;
    }
    auto id = m_syncSources.takeFirst();
    auto account = m_calendars.value(id).value("account").toString();
    if (QDateTime::fromString(m_accounts.value(account).value("retryAt").toString(), Qt::ISODate) >
        QDateTime::currentDateTimeUtc()) {
        syncNext();
        return;
    }
    fetchCalendar(id, [this, id] {
        if (m_calendars.value(id).value("syncError").toString().isEmpty())
            uploadNext(id, [this] { syncNext(); });
        else
            syncNext();
    });
}
void CalendarManager::fetchCalendar(const QString &id, std::function<void()> done) {
    auto c = m_calendars.value(id);
    auto account = c.value("account").toString(), provider = c.value("provider").toString();
    if (provider == "ics") { done(); return; }
    if (provider == "feed" && !m_tokens.contains(account) && !m_transport) {
        loadCredentials(account, [=, this] {
            if (!m_accounts.contains(account)) { done(); return; }
            if (!m_tokens.contains(account)) {
                m_calendars[id]["syncError"] = tr("Calendar account credentials are unavailable.");
                persist(); done(); return;
            }
            fetchCalendar(id, done);
        });
        return;
    }
    auto finish = [=, this](int status, const QByteArray &bytes, const QVariantList &records, bool full) {
        if (status >= 200 && status < 300) {
            m_calendars[id]["syncError"] = "";
            ingest(id, records, full);
            m_calendars[id]["lastSync"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        } else
            m_calendars[id]["syncError"] = errorText(bytes, status);
        persist();
        if (provider == "microsoft" && status == 200) {
            auto from = QDate::fromString(m_preferences.value("rangeFrom").toString(), Qt::ISODate);
            auto to = QDate::fromString(m_preferences.value("rangeTo").toString(), Qt::ISODate);
            if (!from.isValid())
                from = QDate::currentDate().addMonths(-1);
            if (!to.isValid())
                to = from.addMonths(3);
            from = from.addMonths(-1);
            to = to.addMonths(1);
            auto items = std::make_shared<QVariantList>();
            auto next = std::make_shared<std::function<void(QUrl)>>();
            std::weak_ptr<std::function<void(QUrl)>> weak = next;
            *next = [=, this](QUrl url) {
                request(account, "GET", url, {},
                        {{"Prefer", "outlook.body-content-type=\"text\", IdType=\"ImmutableId\""}},
                        [=, this, keep = weak.lock()](int s, QByteArray b, QVariantMap) {
                            const auto page = s == 200 ? eventPage(b, "value") : QJsonObject{};
                            if (page.isEmpty()) {
                                m_calendars[id]["syncError"] = s == 200 ? tr("Invalid calendar server response.") : errorText(b, s);
                                persist();
                                done();
                                return;
                            }
                            auto data = page.toVariantMap();
                            for (const auto &v : data.value("value").toList()) {
                                auto raw = v.toMap();
                                if (raw.value("seriesMasterId").toString().isEmpty())
                                    continue;
                                auto e = CalendarFormat::fromMicrosoft(raw);
                                for (const auto &link : m_links)
                                    if (link.value("calendar") == id &&
                                        link.value("remoteId") == raw.value("seriesMasterId")) {
                                        e["master"] = link.value("event");
                                        break;
                                    }
                                e["originalStart"] = raw.value("originalStart", e.value("start"));
                                e["remoteId"] = raw.value("id");
                                items->append(e);
                            }
                            auto more = data.value("@odata.nextLink").toString();
                            if (!more.isEmpty() && keep) {
                                (*keep)(QUrl(more));
                                return;
                            }
                            m_instances[id] = {{"from", from.toString(Qt::ISODate)},
                                               {"to", to.toString(Qt::ISODate)},
                                               {"events", *items}};
                            persist();
                            done();
                        });
            };
            QUrl url("https://graph.microsoft.com/v1.0/me/calendars/" + esc(c.value("remoteId").toString()) +
                     "/calendarView");
            QUrlQuery query;
            query.addQueryItem("startDateTime", from.toString(Qt::ISODate) + "T00:00:00Z");
            query.addQueryItem("endDateTime", to.toString(Qt::ISODate) + "T00:00:00Z");
            query.addQueryItem("$top", "250");
            url.setQuery(query);
            (*next)(url);
            return;
        }
        done();
    };
    if (provider == "feed") {
        auto url = QUrl(credentials(account).value("url").toString());
        request(account, "GET", url, {},
                c.value("etag").toString().isEmpty() ? QVariantMap{} : QVariantMap{{"If-None-Match", c.value("etag")}},
                [=, this](int s, QByteArray b, QVariantMap h) {
                    if (s == 304) {
                        done();
                        return;
                    }
                    QString error;
                    auto entries = CalendarFormat::readIcs(b, &error);
                    QVariantList rows;
                    for (const auto &v : entries) {
                        auto e = v.toMap();
                        rows.append(QVariantMap{
                            {"remoteId", e.value("uid").toString() + "/" + e.value("recurrenceId").toString()},
                            {"event", e}});
                    }
                    if (s == 200 && !error.isEmpty()) {
                        m_calendars[id]["syncError"] = error;
                        persist();
                        done();
                        return;
                    }
                    if (s == 200)
                        m_calendars[id]["etag"] = h.value("etag");
                    finish(s, b, rows, true);
                });
        return;
    }
    if (provider == "icloud") {
        request(account, "REPORT", QUrl(c.value("url").toString()),
                "<c:calendar-query xmlns:d=\"DAV:\" "
                "xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop><d:getetag/"
                "><c:calendar-data/></d:prop><c:filter><c:comp-filter "
                "name=\"VCALENDAR\"><c:comp-filter "
                "name=\"VEVENT\"/></c:comp-filter></c:filter></c:calendar-query>",
                {{"Depth", "1"}, {"Content-Type", "application/xml"}}, [=](int s, QByteArray b, QVariantMap) {
                    if (s < 200 || s >= 300) { finish(s, b, {}, false); return; }
                    QString error;
                    auto resources = davResponses(b, &error);
                    QVariantList rows;
                    for (const auto &v : resources) {
                        auto resource = v.toMap();
                        const int status = resource.value("errorStatus", resource.value("status").toString().simplified().section(' ', 1, 1).toInt()).toInt();
                        if (status == 401 || status == 403) { finish(status, {}, {}, false); return; }
                        const auto remoteId = QUrl(c.value("url").toString()).resolved(QUrl(resource.value("href").toString())).toString();
                        if (resource.value("calendar-data").toString().trimmed().isEmpty()) {
                            rows.append(QVariantMap{{"remoteId", remoteId}, {"unavailable", true}});
                            continue;
                        }
                        QString parseError;
                        auto events =
                            CalendarFormat::readIcs(resource.value("calendar-data").toString().toUtf8(), &parseError);
                        if (!parseError.isEmpty()) {
                            error = parseError;
                            break;
                        }
                        for (const auto &ev : events) {
                            auto e = ev.toMap();
                            rows.append(QVariantMap{{"remoteId", QUrl(c.value("url").toString())
                                                                     .resolved(QUrl(resource.value("href").toString()))
                                                                     .toString()},
                                                    {"event", e},
                                                    {"etag", resource.value("getetag")},
                                                    {"raw", resource.value("calendar-data")},
                                                    {"exception", e.value("recurrenceId")}});
                        }
                    }
                    finish(error.isEmpty() ? s : 0, error.isEmpty() ? b : json({{"error", QVariantMap{{"message", error}}}}), rows, true);
                });
        return;
    }
    auto rows = std::make_shared<QVariantList>();
    bool full = provider != "google" || c.value("syncToken").toString().isEmpty();
    auto page = std::make_shared<std::function<void(QUrl)>>();
    std::weak_ptr<std::function<void(QUrl)>> weak = page;
    *page = [=, this](QUrl url) {
        request(account, "GET", url, {},
                provider == "microsoft" ? QVariantMap{{"Prefer", "outlook.body-content-type=\"text\", "
                                                                 "IdType=\"ImmutableId\""}}
                                        : QVariantMap{},
                [=, this, keep = weak.lock()](int s, QByteArray b, QVariantMap) {
                    if (s == 410 && provider == "google" && !full) {
                        m_calendars[id].remove("syncToken");
                        fetchCalendar(id, done);
                        return;
                    }
                    if (s != 200) {
                        finish(s, b, {}, false);
                        return;
                    }
                    const QString listKey = provider == "google" ? "items" : "value";
                    const auto page = eventPage(b, listKey);
                    if (page.isEmpty()) {
                        finish(0, json({{"error", QVariantMap{{"message", tr("Invalid calendar server response.")}}}}), {}, false);
                        return;
                    }
                    auto data = page.toVariantMap();
                    for (const auto &v : data.value(listKey).toList()) {
                        auto raw = v.toMap();
                        auto e =
                            provider == "google" ? CalendarFormat::fromGoogle(raw) : CalendarFormat::fromMicrosoft(raw);
                        rows->append(QVariantMap{{"remoteId", raw.value("id")},
                                                 {"event", e},
                                                 {"etag", raw.value(provider == "google" ? "etag" : "@odata.etag")},
                                                 {"raw", raw}});
                    }
                    auto next = data.value(provider == "google" ? "nextPageToken" : "@odata.nextLink").toString();
                    if (!next.isEmpty() && keep) {
                        if (provider == "google") {
                            QUrl u = url;
                            QUrlQuery q(u);
                            q.removeAllQueryItems("pageToken");
                            q.addQueryItem("pageToken", next);
                            u.setQuery(q);
                            (*keep)(u);
                        } else
                            (*keep)(QUrl(next));
                        return;
                    }
                    if (provider == "google")
                        m_calendars[id]["syncToken"] = data.value("nextSyncToken");
                    finish(s, b, *rows, full);
                });
    };
    QUrl url;
    if (provider == "google") {
        url =
            QUrl("https://www.googleapis.com/calendar/v3/calendars/" + esc(c.value("remoteId").toString()) + "/events");
        QUrlQuery q;
        q.addQueryItem("maxResults", "2500");
        q.addQueryItem("showDeleted", "true");
        if (!full)
            q.addQueryItem("syncToken", c.value("syncToken").toString());
        url.setQuery(q);
    } else
        url = QUrl("https://graph.microsoft.com/v1.0/me/calendars/" + esc(c.value("remoteId").toString()) +
                   "/events?$top=250");
    (*page)(url);
}
void CalendarManager::ingest(const QString &calendar, const QVariantList &records, bool full) {
    auto protectedEvent = [this](const QString &id) {
        if (m_events.value(id).value("local").toBool())
            return true;
        int links = 0;
        for (const auto &l : m_links)
            if (l.value("event") == id)
                ++links;
        return links > 1;
    };
    QSet<QString> seen;
    auto provider = m_calendars[calendar].value("provider").toString();
    for (const auto &v : records) {
        auto r = v.toMap(), remote = r.value("event").toMap();
        auto remoteId = r.value("remoteId").toString();
        if (remoteId.isEmpty())
            continue;
        if (r.value("unavailable").toBool()) {
            for (const auto &link : m_links)
                if (link.value("calendar") == calendar && link.value("remoteId") == remoteId) {
                    const auto exception = link.value("exception").toString();
                    seen.insert(remoteId + (exception.isEmpty() ? QString{} : "#" + exception));
                }
            continue;
        }
        auto exception = remote.value("recurrenceId").toString();
        auto identity = remoteId + (exception.isEmpty() ? QString{} : "#" + exception);
        seen.insert(identity);
        QString key;
        for (auto it = m_links.cbegin(); it != m_links.cend(); ++it)
            if (it->value("calendar") == calendar && it->value("remoteId") == remoteId &&
                it->value("exception").toString() == exception) {
                key = it.key();
                break;
            }
        if (key.isEmpty())
            for (auto it = m_links.cbegin(); it != m_links.cend(); ++it)
                if (it->value("calendar") == calendar && it->value("remoteId").toString().isEmpty() &&
                    exception.isEmpty()) {
                    auto e = m_events.value(it->value("event").toString());
                    auto raw = r.value("raw").toMap();
                    if (remoteId == it->value("operationId").toString() ||
                        raw.value("transactionId") == it->value("operationId") ||
                        (!remote.value("uid").toString().isEmpty() && remote.value("uid") == e.value("uid"))) {
                        key = it.key();
                        m_links[key]["remoteId"] = remoteId;
                        m_links[key]["baseline"] = CalendarFormat::content(remote);
                        break;
                    }
                }
        if (remote.value("cancelled").toBool()) {
            if ((provider == "icloud" || provider == "ics") && !exception.isEmpty())
                for (auto &master : m_events)
                    if (master.value("source") == calendar && master.value("uid") == remote.value("uid") &&
                        master.value("recurrenceId").toString().isEmpty()) {
                        auto ex = master.value("exdates").toList();
                        if (!ex.contains(exception))
                            ex.append(exception);
                        master["exdates"] = ex;
                    }
            if (!exception.isEmpty())
                for (const auto &master : m_links)
                    if (master.value("calendar") == calendar &&
                        master.value("remoteId") == remote.value("seriesRemoteId")) {
                        auto masterId = master.value("event").toString();
                        auto ex = m_events[masterId].value("exdates").toList();
                        if (!ex.contains(exception))
                            ex.append(exception);
                        m_events[masterId]["exdates"] = ex;
                    }
            if (!key.isEmpty()) {
                auto eventId = m_links[key].value("event").toString();
                if ((protectedEvent(eventId) || !m_links[key].value("pending").toString().isEmpty()) &&
                    m_links[key].value("pending") != "delete")
                    conflict(key, {}, true);
                else {
                    m_events.remove(eventId);
                    m_links.remove(key);
                }
            }
            continue;
        }
        if (CalendarFormat::dateTime(remote, "start").isNull())
            continue;
        if (key.isEmpty()) {
            key = uid();
            auto id = uid();
            remote["id"] = id;
            remote["local"] = false;
            remote["source"] = calendar;
            remote["version"] = 1;
            remote["destinations"] = QVariantList{calendar};
            remote["organizerCalendar"] = calendar;
            m_events[id] = remote;
            m_links[key] = {{"id", key},
                            {"event", id},
                            {"calendar", calendar},
                            {"remoteId", remoteId},
                            {"exception", exception},
                            {"operationId", uid()},
                            {"baseline", CalendarFormat::content(remote)}};
        }
        auto &l = m_links[key];
        auto eventId = l.value("event").toString();
        auto local = m_events.value(eventId);
        auto baseline = l.value("baseline").toMap();
        auto incoming = CalendarFormat::content(remote);
        auto current = CalendarFormat::content(local);
        if (provider == "microsoft" && !local.value("url").toString().isEmpty()) {
            incoming.remove("url");
            current.remove("url");
            baseline.remove("url");
        }
        // Personal mirrors carry no guest list; invitation state belongs to the
        // organiser copy.
        if (local.value("local").toBool() && local.value("organizerCalendar") != calendar) {
            incoming.remove("guests");
            incoming.remove("organizer");
            current.remove("guests");
            current.remove("organizer");
            baseline.remove("guests");
            baseline.remove("organizer");
        }
        bool collision = false, changed = false;
        auto merged = local;
        for (auto i = incoming.cbegin(); i != incoming.cend(); ++i) {
            if (i.value() == baseline.value(i.key()))
                continue;
            if (current.value(i.key()) != baseline.value(i.key()) && current.value(i.key()) != i.value())
                collision = true;
            else {
                merged[i.key()] = i.value();
                changed |= local.value(i.key()) != i.value();
            }
        }
        l["etag"] = r.value("etag");
        l["raw"] = r.value("raw");
        if (l.value("pending") == "conflict")
            continue;
        if (collision || (l.value("pending") == "delete" && incoming != baseline)) {
            conflict(key, incoming, false);
            continue;
        }
        l["baseline"] = incoming;
        if (changed) {
            for (const auto &f : {"start", "end"})
                if (!merged.value("allDay").toBool())
                    merged[f] = CalendarFormat::dateTime(merged, f)
                                    .toTimeZone(QTimeZone(merged.value("timeZone").toString().toUtf8()))
                                    .toString(Qt::ISODate);
            merged["version"] = local.value("version").toInt() + 1;
            m_events[eventId] = merged;
            for (auto &other : m_links)
                if (other.value("event") == eventId && other.value("calendar") != calendar &&
                    other.value("pending") != "conflict")
                    other["pending"] = "save";
        }
        auto accepted = CalendarFormat::content(m_events[eventId]);
        if (provider == "microsoft" && !local.value("url").toString().isEmpty())
            accepted.remove("url");
        if (local.value("local").toBool() && local.value("organizerCalendar") != calendar) {
            accepted.remove("guests");
            accepted.remove("organizer");
        }
        if (accepted == incoming && l.value("pending") == "save")
            l["pending"] = "";
    }
    // Exclude original occurrences replaced by provider exceptions.
    for (const auto &l : m_links)
        if (l.value("calendar") == calendar && !l.value("exception").toString().isEmpty()) {
            auto exception = m_events.value(l.value("event").toString());
            for (auto &e : m_events)
                if (e.value("source") == calendar && e.value("uid") == exception.value("uid") &&
                    e.value("recurrenceId").toString().isEmpty()) {
                    auto ex = e.value("exdates").toList();
                    if (!ex.contains(l.value("exception")))
                        ex.append(l.value("exception"));
                    e["exdates"] = ex;
                }
        }
    if (full) {
        auto keys = m_links.keys();
        for (const auto &key : keys) {
            auto l = m_links.value(key);
            if (l.value("calendar") != calendar || l.value("remoteId").toString().isEmpty() ||
                seen.contains(l.value("remoteId").toString() + (l.value("exception").toString().isEmpty()
                                                                    ? QString{}
                                                                    : "#" + l.value("exception").toString())))
                continue;
            auto id = l.value("event").toString();
            if (l.value("pending") == "delete") {
                m_links.remove(key);
                continue;
            }
            if (protectedEvent(id) || !l.value("pending").toString().isEmpty())
                conflict(key, {}, true);
            else {
                m_events.remove(id);
                m_links.remove(key);
            }
        }
    }
}
void CalendarManager::uploadNext(const QString &calendar, std::function<void()> done) {
    if (m_interactionActive || m_editSyncTimer.isActive()) {
        m_resync = true;
        done();
        return;
    }
    auto c = m_calendars.value(calendar);
    if (!c.value("enabled").toBool() || !c.value("writable").toBool()) {
        done();
        return;
    }
    auto account = c.value("account").toString(), provider = c.value("provider").toString();
    QString key;
    for (auto it = m_links.cbegin(); it != m_links.cend(); ++it)
        if (it->value("calendar") == calendar &&
            QStringList{"save", "delete"}.contains(it->value("pending").toString())) {
            key = it.key();
            break;
        }
    if (key.isEmpty()) {
        done();
        return;
    }
    auto l = m_links[key];
    auto id = l.value("event").toString();
    auto e = m_events.value(id);
    auto remote = l.value("remoteId").toString();
    bool deleting = l.value("pending") == "delete", creating = remote.isEmpty();
    const bool exceptionDelete = deleting && provider == "icloud" && !l.value("exception").toString().isEmpty();
    if (deleting && creating) {
        m_links.remove(key);
        persist();
        uploadNext(calendar, done);
        return;
    }
    if (provider == "microsoft" && !creating && !deleting) {
        const auto applied = l.value("appliedExdates").toList();
        QString exclusion;
        for (const auto &v : e.value("exdates").toList())
            if (!applied.contains(v)) {
                exclusion = v.toString();
                break;
            }
        if (!exclusion.isEmpty()) {
            auto at = e;
            at["start"] = exclusion;
            auto date = CalendarFormat::dateTime(at, "start");
            const QString endpoint = "https://graph.microsoft.com/v1.0/me/calendars/" +
                                     esc(c.value("remoteId").toString()) + "/events/" + esc(remote);
            QUrl url(endpoint + "/instances");
            QUrlQuery query;
            auto queryDate = date;
            for (const auto &v : m_instances.value(calendar).value("events").toList()) {
                auto cached = v.toMap();
                if (cached.value("master") == id &&
                    QDateTime::fromString(cached.value("originalStart").toString(), Qt::ISODate) == date) {
                    queryDate = CalendarFormat::dateTime(cached, "start");
                    break;
                }
            }
            query.addQueryItem("startDateTime", queryDate.addDays(-1).toUTC().toString(Qt::ISODate));
            query.addQueryItem("endDateTime", queryDate.addDays(2).toUTC().toString(Qt::ISODate));
            query.addQueryItem("$top", "1000");
            url.setQuery(query);
            request(account, "GET", url, {}, {{"Prefer", "IdType=\"ImmutableId\""}},
                    [=, this](int s, QByteArray b, QVariantMap) {
                        if (s != 200) {
                            m_calendars[calendar]["syncError"] = errorText(b, s);
                            persist();
                            done();
                            return;
                        }
                        auto data = QJsonDocument::fromJson(b).toVariant().toMap();
                        if (data.contains("@odata.nextLink")) {
                            m_calendars[calendar]["syncError"] = tr("Too many occurrences in this exclusion range.");
                            persist();
                            done();
                            return;
                        }
                        QString instance;
                        for (const auto &v : data.value("value").toList()) {
                            auto raw = v.toMap();
                            auto normalized = CalendarFormat::fromMicrosoft(raw);
                            auto original = raw.value("originalStart").toString();
                            auto start = original.isEmpty() ? CalendarFormat::dateTime(normalized, "start")
                                                            : QDateTime::fromString(original, Qt::ISODate);
                            if (start == date || (e.value("allDay").toBool() && start.date() == date.date())) {
                                instance = raw.value("id").toString();
                                break;
                            }
                        }
                        auto complete = [=, this](int status, QByteArray bytes, QVariantMap) {
                            if ((status >= 200 && status < 300) || status == 404) {
                                auto ex = m_links[key].value("appliedExdates").toList();
                                ex.append(exclusion);
                                m_links[key]["appliedExdates"] = ex;
                                request(account, "GET", QUrl(endpoint), {}, {{"Prefer", "IdType=\"ImmutableId\""}},
                                        [=, this](int s, QByteArray b, QVariantMap h) {
                                            if (s == 200) {
                                                auto raw = QJsonDocument::fromJson(b).toVariant().toMap();
                                                m_links[key]["etag"] = h.value("etag", raw.value("@odata.etag"));
                                                persist();
                                                uploadNext(calendar, done);
                                            } else {
                                                m_calendars[calendar]["syncError"] = errorText(b, s);
                                                persist();
                                                done();
                                            }
                                        });
                            } else {
                                m_calendars[calendar]["syncError"] = errorText(bytes, status);
                                persist();
                                done();
                            }
                        };
                        if (instance.isEmpty())
                            complete(204, {}, {});
                        else
                            request(account, "DELETE",
                                    QUrl("https://graph.microsoft.com/v1.0/me/events/" + esc(instance)), {}, {},
                                    complete);
                    });
            return;
        }
    }
    if (e.value("local").toBool() && e.value("organizerCalendar") != calendar) {
        e["guests"] = QVariantList{};
        e["organizer"] = "";
    }
    QVariantMap headers;
    QUrl url;
    QByteArray method = deleting ? "DELETE" : creating ? "POST" : "PATCH", body;
    if (!creating && !l.value("etag").toString().isEmpty())
        headers["If-Match"] = l.value("etag");
    if (provider == "google") {
        url = QUrl("https://www.googleapis.com/calendar/v3/calendars/" + esc(c.value("remoteId").toString()) +
                   "/events" + (creating ? "" : "/" + esc(remote)));
        QUrlQuery q;
        q.addQueryItem("sendUpdates",
                       e.value("organizerCalendar") == calendar || !e.value("local").toBool() ? "all" : "none");
        url.setQuery(q);
        auto payload = CalendarFormat::toGoogle(e, l.value("raw").toMap());
        if (creating)
            payload["id"] = l.value("operationId");
        body = json(payload);
    } else if (provider == "microsoft") {
        auto why = CalendarFormat::microsoftUnsupported(e);
        if (!why.isEmpty()) {
            m_calendars[calendar]["syncError"] = why;
            persist();
            done();
            return;
        }
        url = QUrl("https://graph.microsoft.com/v1.0/me/calendars/" + esc(c.value("remoteId").toString()) + "/events" +
                   (creating ? "" : "/" + esc(remote)));
        auto payload = CalendarFormat::toMicrosoft(e, l.value("raw").toMap());
        if (creating)
            payload["transactionId"] = l.value("operationId");
        body = json(payload);
        headers["Prefer"] = "IdType=\"ImmutableId\"";
    } else if (provider == "icloud") {
        method = deleting && !exceptionDelete ? "DELETE" : "PUT";
        if (exceptionDelete)
            e["cancelled"] = true;
        url = creating ? QUrl(c.value("url").toString()).resolved(QUrl(l.value("operationId").toString() + ".ics"))
                       : QUrl(remote);
        headers["Content-Type"] = "text/calendar; charset=utf-8";
        if (creating)
            headers["If-None-Match"] = "*";
        body = CalendarFormat::writeIcs(e, l.value("raw").toString().toUtf8());
    } else {
        done();
        return;
    }
    const bool responding = !deleting && !l.value("response").toString().isEmpty();
    if (!responding && method == "PATCH" && QJsonDocument::fromJson(body).object().isEmpty()) {
        m_links[key]["baseline"] = CalendarFormat::content(e);
        m_links[key]["pending"] = "";
        persist();
        uploadNext(calendar, done);
        return;
    }
    if (responding && provider == "microsoft") {
        const auto response = l.value("response").toString();
        url = QUrl("https://graph.microsoft.com/v1.0/me/events/" + esc(remote) + "/" +
                   (response == "accepted"   ? "accept"
                    : response == "declined" ? "decline"
                                             : "tentativelyAccept"));
        method = "POST";
        body = json({{"sendResponse", true}});
        headers.remove("If-Match");
    }
    request(account, method, url, deleting && !exceptionDelete ? QByteArray{} : body, headers,
            [=, this](int status, QByteArray bytes, QVariantMap response) {
                if (!m_links.contains(key)) {
                    done();
                    return;
                }
                if ((status >= 200 && status < 300) || (deleting && status == 404)) {
                    if (responding) {
                        auto &link = m_links[key];
                        if (link.value("response") == l.value("response")) {
                            link.remove("response");
                            if (link.value("pending") == "save" && m_events.value(id).value("version") == e.value("version"))
                                link["pending"] = "";
                        }
                        link["baseline"] = CalendarFormat::content(e);
                        m_resync = true;
                    } else if (deleting)
                        m_links.remove(key);
                    else {
                        const auto payload = QJsonDocument::fromJson(bytes).object();
                        const auto createdId = payload.value("id").toString();
                        if (creating && provider != "icloud" && createdId.isEmpty()) {
                            m_calendars[calendar]["syncError"] = tr("The calendar server did not return an event ID. Try syncing again.");
                            persist();
                            done();
                            return;
                        }
                        const auto raw = payload.toVariantMap();
                        auto &link = m_links[key];
                        link["remoteId"] = provider == "icloud" ? url.toString() : (createdId.isEmpty() ? remote : createdId);
                        link["etag"] = response.value("etag", raw.value(provider == "google" ? "etag" : "@odata.etag"));
                        link["raw"] = provider == "icloud" ? QVariant(QString::fromUtf8(body)) : QVariant(raw);
                        link["baseline"] = CalendarFormat::content(e);
                        if (m_events.value(id).value("version") == e.value("version") &&
                            link.value("pending") != "delete")
                            link["pending"] = "";
                    }
                    if (provider == "microsoft")
                        m_resync = true;
                    persist();
                    uploadNext(calendar, done);
                } else {
                    m_calendars[calendar]["syncError"] = status == 412 ? tr("The remote event changed. Sync again to "
                                                                            "review the conflict.")
                                                                       : errorText(bytes, status);
                    persist();
                    done();
                }
            });
}
