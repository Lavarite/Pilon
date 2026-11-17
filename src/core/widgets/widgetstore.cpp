#include "widgetstore.h"
#include "widgetplatform.h"
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrlQuery>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr qsizetype maximumBytes = 256 * 1024;
constexpr double maximumGeneration = 9007199254740991.0;
bool matches(const QString &s, const char *p) { return QRegularExpression(QString::fromLatin1(p)).match(s).hasMatch(); }
bool addonId(const QString &s) { return s.size() <= 128 && matches(s, "\\A[a-z][a-z0-9-]*(?:\\.[a-z][a-z0-9-]*)*\\z"); }
bool widgetId(const QString &s) { return matches(s, "\\A[a-z][a-z0-9-]{0,63}\\z"); }
bool id(const QString &s) { return matches(s, "\\A[A-Za-z0-9_-]{1,128}\\z"); }
bool workspace(const QString &s) { return matches(s, "\\A[0-9a-f]{64}\\z"); }
bool map(const QVariant &v) { return v.metaType().id() == QMetaType::QVariantMap; }
bool list(const QVariant &v) { return v.metaType().id() == QMetaType::QVariantList; }
bool keys(const QVariantMap &v, const QStringList &required, const QStringList &optional = {}) {
    if (v.size() < required.size() || v.size() > required.size() + optional.size()) return false;
    for (const auto &k : required) if (!v.contains(k)) return false;
    for (auto i = v.cbegin(); i != v.cend(); ++i) if (!required.contains(i.key()) && !optional.contains(i.key())) return false;
    return true;
}
bool text(const QVariant &v, int limit, bool newline = false) {
    if (v.metaType().id() != QMetaType::QString) return false;
    const auto s = v.toString(); if (s.size() > 2 * limit) return false;
    int points = 0;
    for (qsizetype i = 0; i < s.size(); ++i) {
        char32_t c = s.at(i).unicode();
        if (QChar::isHighSurrogate(c)) {
            if (++i == s.size() || !s.at(i).isLowSurrogate()) return false;
            c = QChar::surrogateToUcs4(s.at(i - 1), s.at(i));
        } else if (QChar::isLowSurrogate(c)) return false;
        if (++points > limit || (QChar::category(c) == QChar::Other_Control && !(newline && c == '\n'))) return false;
    }
    return true;
}
bool colour(const QVariant &v) {
    return v.metaType().id() == QMetaType::QString && matches(v.toString(), "\\A#(?:[0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})\\z");
}
bool number(const QVariant &v, double *result = nullptr) {
    switch (v.metaType().id()) {
    case QMetaType::Int: case QMetaType::UInt: case QMetaType::LongLong: case QMetaType::ULongLong:
    case QMetaType::Float: case QMetaType::Double: break;
    default: return false;
    }
    const auto n = v.toDouble(); if (!std::isfinite(n)) return false;
    if (result) *result = n; return true;
}
bool generationValue(const QVariant &v) {
    double n;
    return number(v, &n) && n >= 1 && n <= maximumGeneration && std::floor(n) == n;
}
bool minuteRange(const QVariant &v) {
    if (!map(v)) return false;
    const auto range = v.toMap(); double start, end;
    return keys(range, {"startMinute", "endMinute"}) && number(range.value("startMinute"), &start)
        && number(range.value("endMinute"), &end) && start >= 0 && start < end && end <= 1440
        && std::floor(start) == start && std::floor(end) == end;
}
bool date(const QVariant &v, qint64 *milliseconds = nullptr) {
    if (v.metaType().id() != QMetaType::QString
        || !matches(v.toString(), "\\A[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\\.[0-9]{1,3})?Z\\z")) return false;
    const auto parsed = QDateTime::fromString(v.toString(), Qt::ISODateWithMs);
    if (!parsed.isValid() || parsed.timeSpec() != Qt::UTC) return false;
    if (milliseconds) *milliseconds = parsed.toMSecsSinceEpoch(); return true;
}
bool clock(const QVariant &v) {
    if (!map(v)) return false;
    const auto value = v.toMap();
    if (!keys(value, {"date", "timeZone"}) || value.value("date").metaType().id() != QMetaType::QString
        || value.value("timeZone").metaType().id() != QMetaType::QString) return false;
    const auto day = value.value("date").toString(), zone = value.value("timeZone").toString();
    const auto parsed = QDate::fromString(day, Qt::ISODate);
    return matches(day, "\\A[0-9]{4}-[0-9]{2}-[0-9]{2}\\z") && parsed.isValid() && parsed.year() >= 1
        && zone.size() <= 128 && matches(zone, "\\A[A-Za-z0-9_+-]+(?:/[A-Za-z0-9_+-]+)*\\z");
}
bool scalar(const QVariant &v) {
    return !v.isValid() || v.metaType().id() == QMetaType::Nullptr || v.metaType().id() == QMetaType::Bool || text(v, 1024) || number(v);
}
bool snapshot(const QVariantMap &v, bool cached, const QString &owner = {}, const QString &widget = {}, const QString &scope = {}) {
    QStringList required{"version", "generation", "updatedAt", "validFrom", "validThrough", "palette", "timeline", "actions"};
    if (cached) required << "workspaceId" << "addonId" << "widgetId";
    if (!keys(v, required) || !number(v.value("version")) || v.value("version").toDouble() != 1 || !generationValue(v.value("generation"))) return false;
    if (cached && (v.value("workspaceId").metaType().id() != QMetaType::QString || v.value("workspaceId").toString() != scope
        || v.value("addonId").metaType().id() != QMetaType::QString || v.value("addonId").toString() != owner
        || v.value("widgetId").metaType().id() != QMetaType::QString || v.value("widgetId").toString() != widget)) return false;
    qint64 from, through, updated;
    if (!date(v.value("validFrom"), &from) || !date(v.value("validThrough"), &through) || !date(v.value("updatedAt"), &updated)
        || from >= through || updated < from || updated >= through) return false;
    if (!map(v.value("palette"))) return false;
    const auto palette = v.value("palette").toMap();
    if (!keys(palette, {"background", "text", "accent"})) return false;
    for (const auto &color : palette)
        if (!colour(color)) return false;
    if (!map(v.value("actions"))) return false;
    const auto actions = v.value("actions").toMap();
    if (!actions.contains("open") || actions.size() > 512) return false;
    for (auto action = actions.cbegin(); action != actions.cend(); ++action) {
        if (!id(action.key()) || !map(action.value()) || action.value().toMap().size() > 8) return false;
        const auto data = action.value().toMap();
        for (auto item = data.cbegin(); item != data.cend(); ++item) if (!id(item.key()) || !scalar(item.value())) return false;
    }
    if (!list(v.value("timeline"))) return false;
    const auto timeline = v.value("timeline").toList();
    if (timeline.isEmpty() || timeline.size() > 64) return false;
    qint64 previous = from; qsizetype rowCount = 0;
    for (qsizetype index = 0; index < timeline.size(); ++index) {
        if (!map(timeline.at(index))) return false;
        const auto frame = timeline.at(index).toMap(); qint64 at;
        if (!keys(frame, {"at", "title", "rows"}, {"heading", "timeGrid", "actionId", "clock"}) || !date(frame.value("at"), &at) || !text(frame.value("title"), 256)
            || at < from || at >= through || (index == 0 ? at != from : at <= previous) || !list(frame.value("rows"))) return false;
        if (frame.contains("heading")) {
            if (!map(frame.value("heading"))) return false;
            const auto heading = frame.value("heading").toMap();
            if (!keys(heading, {"label", "value", "caption"}) || !text(heading.value("label"), 128)
                || !text(heading.value("value"), 16) || !text(heading.value("caption"), 128)) return false;
        }
        if (frame.contains("timeGrid") && !minuteRange(frame.value("timeGrid"))) return false;
        if (frame.contains("clock") && !clock(frame.value("clock"))) return false;
        if (frame.contains("actionId") && (frame.value("actionId").metaType().id() != QMetaType::QString
            || !id(frame.value("actionId").toString()) || !actions.contains(frame.value("actionId").toString()))) return false;
        previous = at; const auto rows = frame.value("rows").toList();
        if (rows.size() > 64 || (rowCount += rows.size()) > 512) return false;
        QSet<QString> ids;
        for (const auto &entry : rows) {
            if (!map(entry)) return false;
            const auto row = entry.toMap();
            if (!keys(row, {"id", "title"}, {"detail", "actionId", "group", "leading", "accent", "timeRange", "allDay"}) || row.value("id").metaType().id() != QMetaType::QString
                || !id(row.value("id").toString()) || ids.contains(row.value("id").toString()) || !text(row.value("title"), 256)
                || (row.contains("detail") && !text(row.value("detail"), 512, true))
                || (row.contains("group") && !text(row.value("group"), 128))
                || (row.contains("leading") && !text(row.value("leading"), 128, true))
                || (row.contains("accent") && !colour(row.value("accent")))
                || (row.contains("timeRange") && !minuteRange(row.value("timeRange")))
                || (row.contains("allDay") && (row.value("allDay").metaType().id() != QMetaType::Bool
                    || (row.value("allDay").toBool() && row.contains("timeRange"))))) return false;
            ids.insert(row.value("id").toString());
            if (row.contains("actionId") && (row.value("actionId").metaType().id() != QMetaType::QString
                || !id(row.value("actionId").toString()) || !actions.contains(row.value("actionId").toString()))) return false;
        }
    }
    return true;
}
bool catalog(const QVariantMap &v, QString *scope, QVariantMap *providers) {
    if (!keys(v, {"version", "workspaceId", "providers"}) || !number(v.value("version")) || v.value("version").toDouble() != 1
        || v.value("workspaceId").metaType().id() != QMetaType::QString || !workspace(v.value("workspaceId").toString()) || !list(v.value("providers"))) return false;
    const auto rows = v.value("providers").toList(); if (rows.size() > 1024) return false;
    QVariantMap result; QHash<QString, int> counts;
    for (const auto &entry : rows) {
        if (!map(entry)) return false;
        const auto row = entry.toMap();
        if (!keys(row, {"addonId", "widgetId", "name"}) || row.value("addonId").metaType().id() != QMetaType::QString || !addonId(row.value("addonId").toString())
            || row.value("widgetId").metaType().id() != QMetaType::QString || !widgetId(row.value("widgetId").toString())
            || !text(row.value("name"), 128) || row.value("name").toString().trimmed().isEmpty()) return false;
        const auto owner = row.value("addonId").toString(), key = owner + '/' + row.value("widgetId").toString();
        if (result.contains(key) || ++counts[owner] > 16) return false;
        result[key] = row;
    }
    *scope = v.value("workspaceId").toString(); *providers = result; return true;
}
bool directory(const QString &path, bool create = false) {
    if (path.isEmpty() || path.contains(QChar::Null) || !QDir::isAbsolutePath(path) || path != QDir::cleanPath(path) || path == QDir::rootPath()) return false;
    QString current;
    for (const auto &part : path.split('/', Qt::SkipEmptyParts)) {
        current += '/' + part; QFileInfo info(current);
        if (info.isSymLink()) return false;
        if (!info.exists() && create) {
            if (!QDir().mkdir(current, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) return false;
            info.refresh();
        }
        if (!info.isDir() || info.isSymLink()) return false;
    }
    return true;
}
bool regularOrAbsent(const QString &file) {
    struct stat status;
    if (::lstat(QFile::encodeName(file).constData(), &status) != 0) return errno == ENOENT;
    return S_ISREG(status.st_mode) && status.st_nlink == 1;
}
QByteArray bytes(const QString &file) {
    const int descriptor = ::open(QFile::encodeName(file).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) return {};
    struct stat status;
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_nlink != 1 || status.st_size > maximumBytes) { ::close(descriptor); return {}; }
    QFile input;
    if (!input.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { ::close(descriptor); return {}; }
    const auto result = input.read(maximumBytes + 1); return result.size() <= maximumBytes ? result : QByteArray{};
}
QVariantMap json(const QString &file) {
    QJsonParseError error; const auto parsed = QJsonDocument::fromJson(bytes(file), &error);
    return error.error == QJsonParseError::NoError && parsed.isObject() ? parsed.object().toVariantMap() : QVariantMap{};
}
bool active(const QVariantMap &v, qint64 now) {
    qint64 from, through;
    return date(v.value("validFrom"), &from) && date(v.value("validThrough"), &through) && now >= from && now < through;
}
}

WidgetStore::WidgetStore(QObject *parent) : WidgetStore(HomeWidgets::directory(), parent) {}
WidgetStore::WidgetStore(const QString &directory, QObject *parent) : QObject(parent), m_directory(directory) {}
bool WidgetStore::available() const { return HomeWidgets::available(); }
bool WidgetStore::canRequestAdd() const { return available() && HomeWidgets::canRequestAdd(); }
bool WidgetStore::fail(const QString &message) { m_error = message; emit changed(); return false; }
bool WidgetStore::ready() { return directory(m_directory, true) || fail(tr("The private widget folder is unavailable or linked.")); }
QString WidgetStore::key(const QString &owner, const QString &widget) const { return addonId(owner) && widgetId(widget) ? owner + '/' + widget : QString{}; }
QString WidgetStore::path(const QString &provider) const {
    return m_directory + '/' + QString::fromLatin1(QCryptographicHash::hash(provider.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".json";
}
bool WidgetStore::save(const QString &file, const QVariantMap &value) {
    if (!directory(m_directory) || QFileInfo(file).absolutePath() != m_directory || !regularOrAbsent(file)) return false;
    const auto data = QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact); if (data.size() > maximumBytes) return false;
    QSaveFile output(file);
    return output.open(QIODevice::WriteOnly) && output.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        && output.write(data) == data.size() && output.commit();
}
bool WidgetStore::currentCatalog() const {
    if (m_workspace.isEmpty() || !directory(m_directory)) return false;
    QString scope; QVariantMap providers;
    return catalog(json(m_directory + "/catalog.json"), &scope, &providers) && scope == m_workspace && providers == m_providers;
}
QVariantMap WidgetStore::read(const QString &provider) const {
    if (!m_providers.contains(provider) || !currentCatalog()) return {};
    const auto owner = m_providers.value(provider).toMap(), value = json(path(provider));
    return snapshot(value, true, owner.value("addonId").toString(), owner.value("widgetId").toString(), m_workspace) ? value : QVariantMap{};
}
bool WidgetStore::configure(const QString &workspaceRoot, const QVariantList &providerList) {
    if (!QDir::isAbsolutePath(workspaceRoot) || workspaceRoot.contains(QChar::Null))
        return fail(tr("Choose an existing workspace before configuring widgets."));
    const auto root = QFileInfo(workspaceRoot).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir())
        return fail(tr("Choose an existing workspace before configuring widgets."));
    const auto scope = QString::fromLatin1(QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex());
    const QVariantMap value{{"version", 1}, {"workspaceId", scope}, {"providers", providerList}};
    QString validatedScope; QVariantMap providers;
    if (!catalog(value, &validatedScope, &providers) || QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact).size() > maximumBytes)
        return fail(tr("The widget provider catalogue is invalid or too large."));
    if (!ready()) return false;
    const auto cataloguePath = m_directory + "/catalog.json";
    if (!regularOrAbsent(cataloguePath)) return fail(tr("The widget catalogue is not a regular private file."));
    QString previousScope; QVariantMap previousProviders; catalog(json(cataloguePath), &previousScope, &previousProviders);
    if (previousScope == scope && previousProviders == providers && m_workspace == scope && m_providers == providers) {
        if (!m_error.isEmpty()) { m_error.clear(); emit changed(); }
        return true;
    }
    if (previousScope != scope || previousProviders != providers)
        if (!save(cataloguePath, value)) return fail(tr("The widget catalogue could not be saved."));
    m_workspace = scope; m_providers = providers; m_error.clear();
    for (auto old = previousProviders.cbegin(); old != previousProviders.cend(); ++old) {
        if (previousScope == scope && providers.contains(old.key())) continue;
        const auto file = path(old.key()); if (!regularOrAbsent(file) || !QFileInfo::exists(file)) continue;
        const auto metadata = old.value().toMap(), previous = json(file);
        if (!snapshot(previous, true, metadata.value("addonId").toString(), metadata.value("widgetId").toString(), previousScope)) continue;
        if (!QFile::remove(file)) m_error = tr("The widget catalogue was updated, but obsolete widget data could not be removed.");
    }
    emit changed(); refreshNative(); return true;
}
bool WidgetStore::publish(const QString &owner, const QString &widget, const QVariantMap &value) {
    const auto provider = key(owner, widget);
    if (provider.isEmpty() || !m_providers.contains(provider) || !currentCatalog()) return fail(tr("This widget provider is not enabled for the current workspace."));
    if (!snapshot(value, false)) return fail(tr("The widget snapshot is invalid or exceeds its limits."));
    if (value.value("generation").toDouble() <= generation(owner, widget)) return fail(tr("Publish a newer widget generation."));
    auto stored = value; stored["workspaceId"] = m_workspace; stored["addonId"] = owner; stored["widgetId"] = widget;
    if (!save(path(provider), stored)) return fail(tr("The widget snapshot could not be saved or is too large."));
    m_error.clear(); emit changed(); refreshNative(); return true;
}
bool WidgetStore::clear(const QString &owner, const QString &widget) {
    const auto provider = key(owner, widget);
    if (provider.isEmpty() || !m_providers.contains(provider) || !currentCatalog()) return fail(tr("This widget provider is not enabled for the current workspace."));
    const auto file = path(provider);
    if (!regularOrAbsent(file) || (QFileInfo::exists(file) && !QFile::remove(file))) return fail(tr("The widget snapshot could not be cleared."));
    m_error.clear(); emit changed(); refreshNative(); return true;
}
double WidgetStore::generation(const QString &owner, const QString &widget) const {
    const auto provider = key(owner, widget); return provider.isEmpty() ? 0 : read(provider).value("generation").toDouble();
}
QVariantMap WidgetStore::resolve(const QUrl &url, qint64 nowMs) {
    if (!url.isValid() || url.toEncoded().size() > 2048 || url.scheme() != "pilon" || url.authority(QUrl::FullyEncoded) != "widget" || url.hasFragment()) {
        fail(tr("The widget action address is invalid.")); return {};
    }
    const auto segments = url.path(QUrl::FullyEncoded).split('/');
    if (segments.size() != 3 || !segments.first().isEmpty()) { fail(tr("The widget action address is invalid.")); return {}; }
    const auto provider = key(segments.at(1), segments.at(2)); QMap<QString, QString> query;
    const auto items = QUrlQuery(url).queryItems(QUrl::FullyDecoded);
    for (const auto &item : items) {
        if (query.contains(item.first) || (item.first != "workspace" && item.first != "action" && item.first != "generation")) {
            fail(tr("The widget action parameters are invalid.")); return {};
        }
        query[item.first] = item.second;
    }
    if (items.size() != 3 || query.value("workspace") != m_workspace || !workspace(query.value("workspace")) || !id(query.value("action"))
        || !matches(query.value("generation"), "\\A[1-9][0-9]{0,15}\\z")) { fail(tr("The widget action parameters are invalid.")); return {}; }
    bool valid = false; const auto requested = query.value("generation").toULongLong(&valid); const auto current = read(provider);
    if (!valid || requested > quint64(maximumGeneration) || current.isEmpty() || requested > current.value("generation").toDouble()
        || !active(current, nowMs == 0 ? QDateTime::currentMSecsSinceEpoch() : nowMs)
        || !current.value("actions").toMap().contains(query.value("action"))) { fail(tr("This widget action is no longer available. Open Pilon to refresh it.")); return {}; }
    m_error.clear(); emit changed();
    return {{"addonId", segments.at(1)}, {"widgetId", segments.at(2)}, {"actionId", query.value("action")},
        {"data", current.value("actions").toMap().value(query.value("action")).toMap()}};
}
bool WidgetStore::requestAdd(const QString &owner, const QString &widget) {
    const auto provider = key(owner, widget);
    if (provider.isEmpty() || !m_providers.contains(provider) || !currentCatalog()) return fail(tr("This widget provider is not enabled for the current workspace."));
    if (!available()) return fail(tr("Home Screen widgets are not available on this platform."));
    if (!canRequestAdd()) return fail(tr("Add the widget from the Home Screen widget gallery."));
    if (!HomeWidgets::requestAdd(owner, widget)) return fail(tr("The Home Screen widget request could not be started."));
    m_error.clear(); emit changed(); emit nativeConfigurationChanged(); return true;
}
QVariantList WidgetStore::activeProviders() const {
    QVariantList result; QSet<QString> seen; if (!currentCatalog()) return result;
    const auto configured = HomeWidgets::activeProviders();
    if (configured.size() > 1024) return result;
    for (const auto &entry : configured) {
        if (!map(entry)) continue; const auto value = entry.toMap();
        if (value.value("workspaceId").metaType().id() != QMetaType::QString || value.value("workspaceId").toString() != m_workspace
            || value.value("addonId").metaType().id() != QMetaType::QString || value.value("widgetId").metaType().id() != QMetaType::QString) continue;
        const auto provider = key(value.value("addonId").toString(), value.value("widgetId").toString());
        if (!m_providers.contains(provider) || seen.contains(provider)) continue;
        seen.insert(provider); result.append(m_providers.value(provider));
    }
    return result;
}
void WidgetStore::refreshNative() { HomeWidgets::refresh(); emit nativeConfigurationChanged(); }
