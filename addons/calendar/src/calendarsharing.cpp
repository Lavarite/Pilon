#include "calendarmanager.h"
#include "calendarformat.h"
#include <QClipboard>
#include <QCryptographicHash>
#include <QDate>
#include <QGuiApplication>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrl>
#include <QTimeZone>
#include <QtConcurrent>

namespace {
QByteArray encode(const QVariantMap &payload) { return QJsonDocument::fromVariant(payload).toJson(QJsonDocument::Compact); }
QString fingerprint(const QVariantMap &payload) { return QString::fromLatin1(QCryptographicHash::hash(encode(payload), QCryptographicHash::Sha256).toHex()); }
QUrl sharingOrigin(const QString &value) {
    QUrl url(value.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() || !url.userInfo().isEmpty()
        || url.hasQuery() || url.hasFragment() || (!url.path().isEmpty() && url.path() != "/")) return {};
    url.setPath({});
    if (url.port() == 443) url.setPort(-1);
    return url;
}
bool validPublisherKey(const QString &key) {
    static const QRegularExpression pattern("\\A[A-Za-z0-9._~+/-]{32,4096}=*\\z");
    return key.size() <= 4096 && pattern.match(key).hasMatch();
}
QString storageError(const QString &message) {
    return message.isEmpty() ? QCoreApplication::translate("CalendarManager", "Sharing changes could not be saved. Check calendar storage.") : message;
}
QString readToken() {
    quint32 words[8];
    QRandomGenerator::system()->fillRange(words);
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(words), sizeof words)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
}

QString CalendarManager::sharingServer() const {
    return sharingOrigin(m_tokens.value("calendar-sharing").value("server").toString()).toString();
}

bool CalendarManager::sharingConfigured() const {
    return !sharingServer().isEmpty() && validPublisherKey(m_tokens.value("calendar-sharing").value("key").toString());
}

void CalendarManager::loadSharingConfiguration() {
    if (m_sharingConfigurationBusy) return;
    m_sharingConfigurationBusy = true;
    m_sharingConfigurationError.clear();
    emit sharingConfigurationChanged();
    loadCredentials("calendar-sharing", [this] {
        m_sharingConfigurationBusy = false;
        emit sharingConfigurationChanged();
    });
}

void CalendarManager::saveSharingConfiguration(const QString &server, const QString &publisherKey) {
    if (m_sharingConfigurationBusy || m_sharingBusy) return;
    const auto origin = sharingOrigin(server);
    if (origin.isEmpty()) {
        m_sharingConfigurationError = tr("Enter an HTTPS server address without a path, query, or sign-in details.");
        emit sharingConfigurationChanged(); return;
    }
    m_sharingConfigurationBusy = true;
    m_sharingConfigurationError.clear();
    emit sharingConfigurationChanged();
    loadCredentials("calendar-sharing", [=, this] {
        const auto previous = m_tokens.value("calendar-sharing");
        const bool sameServer = origin.toString() == sharingServer();
        const auto key = publisherKey.isEmpty() && sameServer ? previous.value("key").toString() : publisherKey;
        if ((!m_shares.isEmpty() && !sameServer) || !validPublisherKey(key)) {
            m_sharingConfigurationError = !m_shares.isEmpty() && !sameServer
                ? tr("Stop sharing all existing links before changing the server.")
                : tr("Enter the publisher key supplied by your sharing server, using at least 32 characters.");
            m_sharingConfigurationBusy = false;
            emit sharingConfigurationChanged(); return;
        }
        const QVariantMap secret{{"server", origin.toString()}, {"key", key}};
        auto watcher = new QFutureWatcher<bool>(this);
        connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, secret] {
            if (watcher->result()) {
                m_tokens["calendar-sharing"] = secret;
                m_sharingConfigurationError.clear();
            } else {
                m_sharingConfigurationError = tr("Pilon could not save sharing settings in the system keyring. Unlock your keyring and try again.");
            }
            watcher->deleteLater();
            m_sharingConfigurationBusy = false;
            emit sharingConfigurationChanged();
        });
        watcher->setFuture(QtConcurrent::run([bytes = encode(secret)]() mutable {
            return keyring("store", "calendar-sharing", &bytes);
        }));
    });
}

bool CalendarManager::clearSharingConfiguration() {
    if (m_sharingConfigurationBusy || m_sharingBusy || !m_shares.isEmpty()) {
        m_sharingConfigurationError = tr("Stop all shared links and wait for sharing to finish before resetting.");
        emit sharingConfigurationChanged(); return false;
    }
    QByteArray unused;
    if (!keyring("clear", "calendar-sharing", &unused)) {
        m_sharingConfigurationError = tr("Unable to remove sharing credentials. Unlock your system keyring and try again.");
        emit sharingConfigurationChanged(); return false;
    }
    m_tokens.remove("calendar-sharing");
    m_sharingConfigurationError.clear();
    emit sharingConfigurationChanged();
    return true;
}

QVariantList CalendarManager::shares() const {
    QVariantList result;
    for (const auto &share : m_shares) result.append(share);
    return result;
}

QVariantMap CalendarManager::shareSnapshot(const QStringList &calendars, const QStringList &fields, const QString &from, const QString &through) const {
    QStringList allowed;
    for (const auto *field : {"title", "location", "description", "url", "guests"})
        if (fields.contains(QString::fromLatin1(field))) allowed.append(QString::fromLatin1(field));
    const auto project = [&](const QVariantMap &source, const QString &identity, bool recurring) {
        QVariantMap shared;
        for (const auto *key : {"start", "end", "allDay", "timeZone", "colour"})
            if (source.contains(key)) shared[key] = source.value(key);
        if (recurring) for (const auto *key : {"recurrence", "exdates", "rdates"})
            if (source.contains(key)) shared[key] = source.value(key);
        for (const auto &key : allowed) if (source.contains(key)) shared[key] = source.value(key);
        if (shared.contains("guests")) {
            QVariantList guests;
            for (const auto &value : shared.value("guests").toList()) {
                const auto guest = value.toMap(); QVariantMap visible;
                for (const auto *key : {"name", "email", "status"}) if (guest.contains(key)) visible[key] = guest.value(key);
                guests.append(visible);
            }
            shared["guests"] = guests;
        }
        shared["id"] = QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
        return shared;
    };
    const QTimeZone zone(m_preferences.value("timeZone").toString().toUtf8());
    const QDateTime begin(QDate::fromString(from, Qt::ISODate), QTime(0, 0), zone);
    const QDateTime end(QDate::fromString(through, Qt::ISODate).addDays(1), QTime(0, 0), zone);
    QVariantList events;
    for (auto it = m_events.cbegin(); it != m_events.cend(); ++it) {
        const auto source = event(it.key());
        if (source.value("deleted").toBool() || source.value("cancelled").toBool()) continue;
        bool selected = (source.value("local", true).toBool() && calendars.contains("pilon"))
            || calendars.contains(source.value("source").toString());
        for (const auto &destination : source.value("destinations").toList())
            selected |= calendars.contains(destination.toString());
        if (!selected) continue;
        bool inRange = !CalendarFormat::occurrences(source, begin, end).isEmpty();
        auto shared = project(source, it.key(), true);
        if (!source.value("recurrence").toString().isEmpty() && !source.value("pending").toBool()) {
            auto calendar = source.value("source").toString();
            if (calendar.isEmpty()) for (const auto &link : m_links)
                if (link.value("event") == it.key() && m_calendars.value(link.value("calendar").toString()).value("provider") == "microsoft") {
                    calendar = link.value("calendar").toString(); break;
                }
            const auto cache = m_instances.value(calendar);
            if (!cache.isEmpty()) {
                QVariantList instances;
                for (const auto &value : cache.value("events").toList()) {
                    const auto instance = value.toMap();
                    if (instance.value("master") != it.key() || instance.value("cancelled").toBool()
                        || instance.value("deleted").toBool()) continue;
                    auto occurrence = source;
                    for (const auto *key : {"title", "start", "end", "allDay", "timeZone", "description", "location", "guests"})
                        if (instance.contains(key)) occurrence[key] = instance.value(key);
                    if (!CalendarFormat::overlaps(occurrence, begin, end)) continue;
                    instances.append(project(occurrence, it.key() + "/" + instance.value("originalStart", instance.value("start")).toString(), false));
                }
                const bool coversRange = cache.value("from").toString() <= from
                    && cache.value("to").toString() >= end.date().toString(Qt::ISODate);
                inRange = coversRange ? !instances.isEmpty() : inRange || !instances.isEmpty();
                shared["instances"] = QVariantMap{{"from", cache.value("from")}, {"to", cache.value("to")}, {"events", instances}};
            }
        }
        if (inRange) events.append(shared);
    }
    QVariantMap preferences;
    for (const auto *key : {"timeZone", "firstDay", "view", "eventLayout"})
        if (m_preferences.contains(key)) preferences[key] = m_preferences.value(key);
    return {{"fields", allowed}, {"preferences", preferences}, {"events", events}, {"from", from}, {"through", through}};
}

void CalendarManager::shareRequest(const QByteArray &method, const QString &id, const QVariantMap &payload, Reply done, bool create) {
    loadCredentials("calendar-sharing", [=, this] {
        const auto secret = credentials("calendar-sharing");
        const auto server = sharingOrigin(secret.value("server").toString());
        const auto key = secret.value("key").toString().toUtf8();
        if (server.isEmpty() || !validPublisherKey(QString::fromUtf8(key))) {
            m_sharingError = tr("Configure schedule sharing in Settings before creating a link.");
            done(0, {}, {{"error", m_sharingError}}); return;
        }
        auto url = server;
        url.setPath("/api/shares" + (id.isEmpty() ? QString{} : "/" + QString::fromUtf8(QUrl::toPercentEncoding(id))));
        if (create) url.setQuery("create=true");
        QVariantMap headers{{"Authorization", "Bearer " + QString::fromUtf8(key)}, {"Content-Type", "application/json"}};
        if (m_transport) { m_transport("calendar-sharing", method, url, encode(payload), headers, done); return; }
        QNetworkRequest request(url);
        request.setTransferTimeout(30000);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Authorization", "Bearer " + key);
        auto reply = m_network.sendCustomRequest(request, method, method == "DELETE" ? QByteArray{} : encode(payload));
        connect(reply, &QNetworkReply::finished, this, [reply, done] {
            const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto bytes = reply->readAll(); reply->deleteLater(); done(status, bytes, {});
        });
    });
}

void CalendarManager::createShare(const QStringList &calendars, const QStringList &fields, const QString &from, const QString &through, const QString &name) {
    if (!m_active) { m_sharingError = tr("Enable Calendar to publish a shared link."); emit sharingChanged(); return; }
    if (m_sharingBusy || m_sharingConfigurationBusy) return;
    if (name.trimmed().isEmpty()) { m_sharingError = tr("Enter a name for the link."); emit sharingChanged(); return; }
    if (calendars.isEmpty()) { m_sharingError = tr("Select at least one calendar."); emit sharingChanged(); return; }
    const auto first = QDate::fromString(from, Qt::ISODate), last = QDate::fromString(through, Qt::ISODate);
    if (!first.isValid() || !last.isValid() || last < first) {
        m_sharingError = tr("Choose a valid date range."); emit sharingChanged(); return;
    }
    for (const auto &id : calendars) if (!m_calendars.contains(id)) {
        m_sharingError = tr("A selected calendar is no longer available."); emit sharingChanged(); return;
    }
    m_sharingBusy = true; m_sharingError.clear(); emit sharingChanged();
    loadCredentials("calendar-sharing", [=, this] {
        if (!sharingConfigured()) {
            m_sharingError = tr("Configure schedule sharing in Settings before creating a link.");
            m_sharingBusy = false; emit sharingChanged(); return;
        }
        const auto id = readToken();
        const auto payload = shareSnapshot(calendars, fields, from, through);
        m_shares[id] = {{"id", id}, {"name", name.trimmed()}, {"url", ""}, {"calendars", calendars},
            {"fields", fields}, {"from", from}, {"through", through}, {"pending", true}, {"publication", payload}};
        // The token must survive a lost response or a process exit after the server writes.
        if (!persist()) {
            m_shares.remove(id);
            m_sharingError = storageError(m_error);
            m_sharingBusy = false; emit sharingChanged(); return;
        }
        publishShare(id);
    });
}

void CalendarManager::publishShare(const QString &id) {
    if (!m_active) return;
    const auto share = m_shares.value(id);
    const bool pending = share.value("pending").toBool();
    // Creation retries reuse the persisted payload and token.
    const auto payload = pending ? share.value("publication").toMap()
        : shareSnapshot(share.value("calendars").toStringList(), share.value("fields").toStringList(),
                        share.value("from").toString(), share.value("through").toString());
    m_sharingBusy = true; emit sharingChanged();
    shareRequest("PUT", id, payload, [=, this](int status, QByteArray bytes, QVariantMap metadata) {
        auto &current = m_shares[id];
        const auto previous = current;
        const bool acknowledged = status >= 200 && status < 300
            && (!pending || QJsonDocument::fromJson(bytes).object().value("id").toString() == id);
        bool created = false;
        if (acknowledged) {
            current.remove("error"); current.remove("unavailable");
            current["digest"] = fingerprint(payload);
            if (pending) {
                current.remove("pending"); current.remove("publication");
                auto url = QUrl(sharingServer()); url.setPath("/calendar"); url.setFragment(id);
                current["url"] = url.toString();
            }
            if (persist()) {
                m_shareRetryAfter.remove(id);
                created = pending;
            } else {
                current = previous;
                current["error"] = storageError(m_error);
                m_shareRetryAfter[id] = QDateTime::currentDateTimeUtc().addSecs(30);
            }
        } else {
            const bool permanent = status == 404 || status == 409;
            current["unavailable"] = permanent;
            current["error"] = !metadata.value("error").toString().isEmpty() ? metadata.value("error").toString()
                : pending && status == 404
                ? tr("Update your sharing server before creating links. Stop this pending link, then create it again.")
                : permanent ? tr("This shared link is unavailable. Stop sharing it, then create a new link.")
                : tr("This shared link could not update. Pilon will retry.");
            if (permanent) m_shareRetryAfter.remove(id);
            else m_shareRetryAfter[id] = QDateTime::currentDateTimeUtc().addSecs(30);
            if (permanent && !persist()) current["error"] = storageError(m_error);
        }
        m_sharingError.clear();
        for (const auto &record : m_shares)
            if (!record.value("error").toString().isEmpty()) { m_sharingError = record.value("error").toString(); break; }
        m_sharingBusy = false; emit sharingChanged();
        if (created) emit shareCreated(id);
        if (m_active) m_shareTimer.start(600);
    }, pending);
}

bool CalendarManager::renameShare(const QString &id, const QString &name) {
    if (!m_active) { m_sharingError = tr("Enable Calendar to update a shared link."); emit sharingChanged(); return false; }
    if (!m_shares.contains(id) || name.trimmed().isEmpty()) return false;
    const auto previous = m_shares[id];
    m_shares[id]["name"] = name.trimmed();
    const bool saved = persist();
    if (!saved) m_shares[id] = previous;
    m_sharingError = saved ? QString{} : storageError(m_error);
    emit sharingChanged();
    return saved;
}

void CalendarManager::revokeShare(const QString &id) {
    if (m_sharingBusy || !m_shares.contains(id)) return;
    m_sharingBusy = true; m_sharingError.clear(); emit sharingChanged();
    shareRequest("DELETE", id, {}, [=, this](int status, QByteArray, QVariantMap) {
        if ((status >= 200 && status < 300) || status == 404) {
            const auto previous = m_shares.take(id);
            if (!persist()) {
                m_shares[id] = previous;
                m_shares[id]["unavailable"] = true;
                m_shares[id]["error"] = storageError(m_error);
                m_sharingError = storageError(m_error);
            }
            else m_shareRetryAfter.remove(id);
        }
        else if (m_sharingError.isEmpty()) m_sharingError = tr("Unable to stop sharing. Try again.");
        m_sharingBusy = false; emit sharingChanged(); if (m_active) m_shareTimer.start(600);
    });
}

void CalendarManager::copyShareLink(const QString &id) const {
    const auto share = m_shares.value(id);
    if (!share.value("pending").toBool() && !share.value("unavailable").toBool() && !share.value("url").toString().isEmpty())
        QGuiApplication::clipboard()->setText(share.value("url").toString());
}

void CalendarManager::refreshShares() {
    if (!m_active) return;
    if (m_sharingBusy || m_sharingConfigurationBusy) { if (m_active) m_shareTimer.start(600); return; }
    const auto now = QDateTime::currentDateTimeUtc();
    QDateTime nextRetry;
    for (auto it = m_shares.cbegin(); it != m_shares.cend(); ++it) {
        const auto id = it.key();
        if (it->value("unavailable").toBool()) continue;
        const auto retry = m_shareRetryAfter.value(id);
        if (retry > now) {
            if (!nextRetry.isValid() || retry < nextRetry) nextRetry = retry;
            continue;
        }
        if (it->value("pending").toBool()) { publishShare(id); return; }
        const auto payload = shareSnapshot(it->value("calendars").toStringList(), it->value("fields").toStringList(), it->value("from").toString(), it->value("through").toString());
        const auto digest = fingerprint(payload);
        if (it->value("digest").toString() == digest) continue;
        publishShare(id);
        return;
    }
    if (m_active && nextRetry.isValid()) m_shareTimer.start(int(qMax<qint64>(1, now.msecsTo(nextRetry))));
}
