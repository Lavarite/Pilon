#include "piconfpreferences.h"
#include "piconftransaction.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUrl>
#include <memory>

namespace {
using Files = PiconfDocument::Files;
const QString hostStore = QStringLiteral(".pilon/settings.ini");
const QString baselineFile = QStringLiteral(".pilon/piconf-baseline.json");
const QString receiptsFile = QStringLiteral(".pilon/addons/registry.json");
const QStringList builtins{"assistant", "calendar", "devices", "files", "git", "notes", "ssh"};

bool validId(const QString &id) {
    static const QRegularExpression external(R"(\A[a-z][a-z0-9-]*(?:\.[a-z][a-z0-9-]*)+\z)");
    return builtins.contains(id) || (id.size() <= 128 && external.match(id).hasMatch());
}
bool validKey(const QString &key) {
    static const QRegularExpression names(R"(\A[a-zA-Z0-9][a-zA-Z0-9_.-]{0,127}\z)");
    return names.match(key).hasMatch();
}
QString addonFile(const QString &id) {
    return "addon-data/" + id + "/config.piconf";
}
QString addonStore(const QString &id) {
    return "addon-data/" + id + "/settings.ini";
}

bool readExisting(const QString &root, const QString &name, Files &files, QString &error) {
    const QString path = root + '/' + name;
    for (QString part = path; part != root; part = QFileInfo(part).absolutePath()) {
        if (QFileInfo(part).isSymLink()) {
            error = QObject::tr("Configuration path contains a symbolic link: %1").arg(name);
            return false;
        }
    }
    QFile f(path);
    if (!f.exists()) return true;
    if (!QFileInfo(f).isFile() || f.size() > 30 * 1024 * 1024 || !f.open(QIODevice::ReadOnly)) {
        error = QObject::tr("Cannot read configuration store %1.").arg(name);
        return false;
    }
    files[name] = f.readAll();
    if (f.error() == QFileDevice::NoError) return true;
    error = f.errorString();
    return false;
}

bool jsonObject(const QByteArray &bytes, QVariantMap &out, const QString &label, QString &error) {
    const auto doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject()) {
        error = QObject::tr("Invalid configuration metadata in %1.").arg(label);
        return false;
    }
    out = doc.object().toVariantMap();
    return true;
}
QVariant normalized(const QVariant &value) {
    return QJsonValue::fromVariant(value).toVariant();
}

enum class Codec { Plain, Colour, JsonText, JsonBytes, Path, Bookmarks, Appearance, Providers, Servers };
struct Field {
    QString store, native, file;
    QStringList keys;
    QVariant fallback;
    Codec codec = Codec::Plain;
    QString identity() const { return store + ':' + native; }
};
QVariant nested(QVariant value, const QStringList &path) {
    for (const QString &part : path) value = value.toMap().value(part);
    return value;
}
QString portablePath(const QString &root, const QString &path) {
    return path == root                  ? QStringLiteral("@workspace")
           : path.startsWith(root + '/') ? "@workspace" + path.mid(root.size())
                                         : path;
}

QString nativePath(const QString &root, const QString &path) {
    return path == "@workspace" || path.startsWith("@workspace/") ? root + path.mid(10) : path;
}
QVariant remapPaths(Codec codec, const QVariant &value, const QString &root, bool toNative) {
    const auto mapPath = [&](const QString &path) { return toNative ? nativePath(root, path) : portablePath(root, path); };
    if (codec == Codec::Bookmarks) {
        auto entries = value.toList();
        for (auto &entry : entries) {
            auto item = entry.toMap();
            if (item.value("deviceId") == "local") {
                item["path"] = mapPath(item.value("path").toString());
                entry = item;
            }
        }
        return entries;
    }
    auto rules = value.toMap();
    if (!rules.contains("paths")) return rules;
    const auto paths = rules.value("paths").toMap();
    QVariantMap mapped;
    for (auto it = paths.cbegin(); it != paths.cend(); ++it) {
        auto key = it.key();
        auto entry = QJsonDocument::fromJson(key.toUtf8()).array();
        if (entry.size() == 4 && entry.at(0) == "local") {
            entry[2] = mapPath(entry.at(2).toString());
            key = QString::fromUtf8(QJsonDocument(entry).toJson(QJsonDocument::Compact));
        }
        mapped[key] = it.value();
    }
    rules["paths"] = mapped;
    return rules;
}
const QStringList &providerKeys(Codec codec) {
    static const QStringList git{"id", "type", "name", "host", "sshKey", "credentialUrl", "credentialUsername"};
    static const QStringList assistant{"id",      "name",   "baseUrl", "remoteModel", "protocol",
                                       "context", "vision", "audio",   "video",       "modalityOverrides"};
    return codec == Codec::Providers ? git : assistant;
}
QVariantList selectProviders(const QVariantList &rows, Codec codec) {
    QVariantList result;
    for (const auto &row : rows) {
        QVariantMap selected;
        const auto input = row.toMap();
        for (const auto &key : providerKeys(codec))
            if (input.contains(key)) selected.insert(key, input[key]);
        result.append(selected);
    }
    return result;
}
QVariant asPortable(const Field &field, QVariant raw, const QString &root) {
    if (!raw.isValid()) return field.fallback;
    switch (field.codec) {
    case Codec::Colour:
        return raw.value<QColor>().name(QColor::HexArgb);
    case Codec::JsonBytes:
    case Codec::JsonText:
        return QJsonDocument::fromJson(raw.toByteArray()).toVariant();
    case Codec::Path:
        return portablePath(root, raw.toString());
    case Codec::Bookmarks:
        return normalized(remapPaths(field.codec, raw, root, false));
    case Codec::Appearance:
        return remapPaths(field.codec, QJsonDocument::fromJson(raw.toByteArray()).toVariant(), root, false);
    case Codec::Providers:
    case Codec::Servers:
        return normalized(selectProviders(raw.toList(), field.codec));
    case Codec::Plain:
        break;
    }
    if (field.fallback.typeId() == QMetaType::QVariantList && raw.typeId() == QMetaType::QString)
        return normalized(raw.toStringList());
    if (field.fallback.isValid() && raw.metaType() != field.fallback.metaType() && raw.canConvert(field.fallback.metaType()))
        raw.convert(field.fallback.metaType());
    return normalized(raw);
}
QVariant asNative(const Field &field, const QVariant &value, const QVariant &before, const QString &root) {
    if (!value.isValid()) return {};
    switch (field.codec) {
    case Codec::Colour:
        return QColor(value.toString());
    case Codec::JsonText:
        return QString::fromUtf8(QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact));
    case Codec::JsonBytes:
        return QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact);
    case Codec::Path:
        return nativePath(root, value.toString());
    case Codec::Bookmarks:
        return remapPaths(field.codec, value, root, true);
    case Codec::Appearance:
        return QString::fromUtf8(
            QJsonDocument::fromVariant(remapPaths(field.codec, value, root, true)).toJson(QJsonDocument::Compact));
    case Codec::Providers:
    case Codec::Servers: {
        QVariantList result;
        const QString endpoint = field.codec == Codec::Servers ? "baseUrl" : "host";
        for (const auto &row : value.toList()) {
            auto item = row.toMap();
            // Keyring entries belong to a local server. A restored ID alone cannot grant an API key.
            if (field.codec == Codec::Servers) item.insert("hasKey", false);
            for (const auto &oldRow : before.toList()) {
                const auto old = oldRow.toMap();
                if (old.value("id") != item.value("id") || old.value(endpoint) != item.value(endpoint)) continue;
                if (field.codec == Codec::Servers) item.remove("hasKey");
                for (auto it = old.cbegin(); it != old.cend(); ++it)
                    if (!providerKeys(field.codec).contains(it.key())) item.insert(it.key(), it.value());
                break;
            }
            result.append(item);
        }
        return result;
    }
    case Codec::Plain:
        return value;
    }
    return {};
}

struct Stores {
    QString root;
    Files before;
    QTemporaryDir scratch;
    QMap<QString, std::shared_ptr<QSettings>> settings;
    QVariantMap baseline, receipts;

    bool open(const QString &name, QString &error) {
        if (settings.contains(name)) return true;
        if (!readExisting(root, name, before, error)) return false;
        const QString path = scratch.filePath(QString::number(settings.size()) + ".ini");
        QFile copy(path);
        if (!scratch.isValid() || !copy.open(QIODevice::WriteOnly) ||
            copy.write(before.value(name)) != before.value(name).size()) {
            error = QObject::tr("Cannot stage preferences for %1.").arg(name);
            return false;
        }
        copy.close();
        auto store = std::make_shared<QSettings>(path, QSettings::IniFormat);
        store->allKeys();
        if (store->status() != QSettings::NoError) {
            error = QObject::tr("Cannot read %1.").arg(name);
            return false;
        }
        settings.insert(name, store);
        return true;
    }
    bool initialize(QString &error) {
        if (!open(hostStore, error) || !readExisting(root, baselineFile, before, error) ||
            !readExisting(root, receiptsFile, before, error))
            return false;
        if (before.contains(baselineFile) && !jsonObject(before[baselineFile], baseline, baselineFile, error)) return false;
        if (before.contains(receiptsFile)) {
            QVariantMap registry;
            if (!jsonObject(before[receiptsFile], registry, receiptsFile, error)) return false;
            receipts = registry.value("owners").toMap();
            before.remove(receiptsFile);
        }
        return true;
    }
    bool collect(const QSet<QString> &changed, Files &updated, QString &error) {
        for (const auto &id : changed) {
            const auto store = settings[id];
            store->sync();
            QFile f(store->fileName());
            if (store->status() != QSettings::NoError || !f.open(QIODevice::ReadOnly)) {
                error = QObject::tr("Cannot stage %1.").arg(id);
                return false;
            }
            updated[id] = f.readAll();
        }
        const auto bytes = QJsonDocument::fromVariant(baseline).toJson(QJsonDocument::Compact);
        if (before.value(baselineFile) != bytes) updated[baselineFile] = bytes;
        return true;
    }
};

// Legacy add-ons store preferences beside documents or events. Read only their
// preference record so a disabled add-on can be exported before its first launch.
bool seedLegacyOptions(Stores &stores, QSet<QString> &changed, QString &error) {
    if (stores.baseline.contains("$legacyOptions")) return true;
    const auto host = stores.settings.value(hostStore);
    for (const QString &id : {QStringLiteral("notes"), QStringLiteral("calendar")}) {
        const auto key = id + "/portablePreferences";
        if (host->contains(key)) continue;
        QVariantMap options;
        if (id == "notes") {
            const QString library = host->value("notes/directory", stores.root + "/Notes").toString();
            QFile source(QDir(library).filePath(".settings.json"));
            if (!source.exists()) continue;
            if (QFileInfo(source).isSymLink() || source.size() > PiconfDocument::MaxFileBytes ||
                !source.open(QIODevice::ReadOnly)) {
                error = QObject::tr("Cannot read the Notes preference file at %1.").arg(source.fileName());
                return false;
            }
            QVariantMap saved;
            if (!jsonObject(source.readAll(), saved, source.fileName(), error)) return false;
            for (const auto &name :
                 {"paper", "recognizeShapes", "showPageOutlines", "drawWithFinger", "pressureSensitive", "pens"})
                if (saved.contains(name)) options[name] = saved[name];
        } else {
            const QString database = stores.root + "/.pilon/calendar/calendar.sqlite";
            if (!QFileInfo::exists(database)) continue;
            if (QFileInfo(database).canonicalFilePath() != database) {
                error = QObject::tr("Cannot read Calendar preferences through a symbolic link.");
                return false;
            }
            const auto connection = QUuid::createUuid().toString();
            const auto release = qScopeGuard([&] { QSqlDatabase::removeDatabase(connection); });
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setConnectOptions("QSQLITE_OPEN_READONLY");
            db.setDatabaseName(database);
            if (!db.open()) {
                error = db.lastError().text();
                return false;
            }
            QSqlQuery query(db);
            if (!query.exec("SELECT data FROM records WHERE kind='preferences' AND id='main'")) {
                error = query.lastError().text();
                return false;
            }
            if (!query.next()) continue;
            if (!jsonObject(query.value(0).toByteArray(), options, database, error)) return false;
        }
        if (!options.isEmpty()) {
            host->setValue(key, options);
            changed.insert(hostStore);
        }
    }
    stores.baseline.insert("$legacyOptions", true);
    return true;
}

QList<Field> fields(const PiconfDocument &doc, Stores &stores, QString &error) {
    QList<Field> result;
    const auto main = PiconfDocument::MainPath;
    const auto host = [&](QString key, QString group, QString name, QVariant fallback, Codec codec = Codec::Plain) {
        result.append({hostStore, key, main, {group, name}, fallback, codec});
    };
    host("Pilon/glassFrostiness", "appearance", "frostiness", 78);
    host("Pilon/colourModeIndex", "appearance", "colourMode", 0);
    host("Pilon/customBackgroundColour", "appearance", "background", "#ff121212", Codec::Colour);
    host("Pilon/customSidebarColour", "appearance", "sidebar", "#ff050505", Codec::Colour);
    host("Pilon/customAccentColour", "appearance", "accent", "#ff3b82f6", Codec::Colour);
    host("Pilon/customTextColour", "appearance", "text", "#fff5f5f5", Codec::Colour);
    host("Pilon/customWarningColour", "appearance", "warning", "#fff0a43c", Codec::Colour);
    host("Pilon/touchMode", "application", "touchMode", false);
    host("Pilon/keyboardShortcutHints", "application", "keyboardShortcutHints", true);
    host("app/backgroundEnabled", "application", "backgroundEnabled", false);
    host("app/trayEnabled", "application", "trayEnabled", true);
    host("keybindings/overrides", "shortcuts", "overrides", QVariantMap{});
    host("keybindings/stylus", "shortcuts", "stylus", QVariantMap{});
    host("addons/sidebarOrder", "sidebar", "order", QVariantList{});
    host("addons/hiddenPages", "sidebar", "hidden", QVariantList{});
    const auto declarations = doc.values().value("addons").toMap();
    for (auto it = declarations.cbegin(); it != declarations.cend(); ++it) {
        const QString id = it.key(), file = doc.addonFile(id), store = addonStore(id);
        if (!stores.open(store, error)) return {};
        result.append({hostStore,
                       "addons/enabled/" + id,
                       main,
                       {"addons", id, "enabled"},
                       builtins.contains(id) && id != "git" && id != "devices"});
        const auto pref = [&](const QString &key, const QString &name, const QVariant &fallback = {},
                              Codec codec = Codec::Plain) {
            result.append({hostStore, key, file, {"preferences", name}, fallback, codec});
        };
        if (id == "files") {
            pref("Pilon/showHiddenFiles", "showHidden", true);
            pref("files/bookmarks", "bookmarks", QVariantList{}, Codec::Bookmarks);
        } else if (id == "ssh") {
            pref("Pilon/defaultSshUsername", "defaultUsername", QString{});
            pref("Pilon/deviceUsernamesJson", "usernames", QVariantMap{}, Codec::JsonText);
            pref("ssh/savedDevices", "devices", QVariantList{}, Codec::JsonBytes);
        } else if (id == "devices") {
            pref("tailscale/includedDevices", "includedDevices", QVariantList{});
            pref("tailscale/deviceSelectionInitialized", "selectionInitialized", false);
        } else if (id == "git") {
            pref("GitHub/commitName", "commitName", QString{});
            pref("GitHub/commitEmail", "commitEmail", QString{});
            pref("GitHub/cloneDirectory", "cloneDirectory", {}, Codec::Path);
            pref("Git/providers", "providers", {}, Codec::Providers);
            pref("Git/activeProvider", "activeProvider");
        } else if (id == "notes" || id == "calendar") {
            pref(id + "/directory", "directory", {}, Codec::Path);
            pref(id + "/portablePreferences", "options");
        } else if (id == "assistant") {
            pref("assistant/apiServers", "servers", {}, Codec::Servers);
            pref("assistant/model", "model");
            pref("assistant/effort", "effort", 0);
            pref("assistant/yolo", "automaticActions", false);
            pref("assistant/styleInstructions", "instructions", QString{});
            pref("assistant/voiceMode", "voiceMode", false);
            pref("assistant/voicePrompt", "voicePrompt");
            pref("assistant/voiceSpeed", "voiceSpeed", 5);
            pref("assistantSidebar/width", "sidebarWidth");
            auto reasoning = doc.values(file).value("reasoning").toMap().keys();
            for (const auto &key : stores.settings[hostStore]->allKeys())
                if (key.startsWith("assistant/reasoning/"))
                    reasoning << key.sliced(QStringLiteral("assistant/reasoning/").size());
            reasoning.removeDuplicates();
            for (const auto &key : reasoning)
                result.append({hostStore, "assistant/reasoning/" + key, file, {"reasoning", key}, {}});
        }
        const QString prefix = "addons/integrations/" + id + '/';
        auto integrations = doc.values(file).value("integrations").toMap().keys();
        for (const auto &key : stores.settings[hostStore]->allKeys())
            if (key.startsWith(prefix)) integrations << key.mid(prefix.size());
        integrations.removeDuplicates();
        for (const auto &name : integrations)
            if (validKey(name)) result.append({hostStore, prefix + name, file, {"integrations", name}, {}});
        auto keys = stores.receipts.value(id).toMap().value("portableSettings").toStringList();
        if (id == "files") keys << "appearance";
        if (id == "ssh") keys << "trustedX11Forwarding";
        keys.removeDuplicates();
        for (const auto &key : keys) {
            if (!validKey(key)) continue;
            const auto codec = id == "files" && key == "appearance" ? Codec::Appearance : Codec::Plain;
            result.append({store,
                           key,
                           file,
                           {"settings", key},
                           id == "ssh" && key == "trustedX11Forwarding" ? QVariant(false) : QVariant{},
                           codec});
        }
    }
    return result;
}

bool ensureDeclarations(PiconfDocument &doc, Stores &stores, QString &error) {
    const auto declarations = doc.values().value("addons").toMap();
    auto ids = builtins;
    for (auto it = stores.receipts.cbegin(); it != stores.receipts.cend(); ++it)
        if (validId(it.key()) && !it.value().toMap().value("removed").toBool()) ids << it.key();
    ids.removeDuplicates();
    QList<PiconfDocument::Edit> edits;
    Files additions;
    for (const auto &id : ids) {
        const auto info = stores.receipts.value(id).toMap();
        const QString source = info.value("source", builtins.contains(id) ? "builtin" : "local").toString();
        const QString sourceMarker = "$source/" + id;
        if (declarations.contains(id)) {
            const auto old = stores.baseline.value(sourceMarker);
            if (!old.isValid()) continue;
            const auto declared = declarations.value(id).toMap().value("source");
            if (old != declared)
                stores.baseline.remove(sourceMarker);
            else if (info.contains("source") && declared != source) {
                edits.append({PiconfDocument::MainPath, {"addons", id, "source"}, source});
                stores.baseline.insert(sourceMarker, source);
            }
            continue;
        }
        const QString path = addonFile(id);
        if (!readExisting(stores.root, path, additions, error)) return false;
        if (!additions.contains(path)) additions[path] = PiconfDocument::encode({{"format", 1}});
        stores.baseline.insert(sourceMarker, source);
        for (const auto &[key, value] : QVariantMap{{"source", source}, {"config", "../" + path}}.asKeyValueRange())
            edits.append({PiconfDocument::MainPath, {"addons", id, key}, value});
    }
    return edits.isEmpty() || doc.apply(edits, additions, error);
}

bool supported(const Field &field, const QVariant &value) {
    if (!value.isValid()) return true;
    const auto type = QJsonValue::fromVariant(value).type();
    if (field.fallback.isValid() && type != QJsonValue::fromVariant(field.fallback).type()) return false;
    if (field.codec == Codec::Appearance) return type == QJsonValue::Object;
    if (field.codec == Codec::Colour) return QColor::isValidColorName(value.toString());
    if (field.codec == Codec::Path) {
        const auto path = value.toString();
        return type == QJsonValue::String && (path.isEmpty() || QDir::isAbsolutePath(path) || path == "@workspace" ||
                                              (path.startsWith("@workspace/") && QDir::cleanPath(path) == path));
    }
    if (field.codec == Codec::Servers || field.codec == Codec::Providers) {
        if (type != QJsonValue::Array) return false;
        QSet<QString> ids;
        for (const auto &row : value.toList()) {
            const auto entry = row.toMap();
            const auto id = entry.value("id").toString();
            if (id.isEmpty() || ids.contains(id)) return false;
            ids.insert(id);
            const auto overrides = entry.value("modalityOverrides").toMap();
            for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
                if (!QStringList{"vision", "audio", "video"}.contains(it.key()) ||
                    QJsonValue::fromVariant(it.value()).type() != QJsonValue::Double)
                    return false;
            if (field.codec == Codec::Servers && !entry.contains("baseUrl")) return false;
            for (const auto &key : entry.keys())
                if (!providerKeys(field.codec).contains(key)) return false;
            for (const auto &key : {"baseUrl", "credentialUrl"}) {
                const QUrl url(entry.value(key).toString());
                if (!url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) return false;
                if (entry.contains(key) &&
                    (url.host().isEmpty() || !url.isValid() ||
                     (url.scheme() != "https" && (field.codec != Codec::Servers || url.scheme() != "http"))))
                    return false;
            }
        }
    }
    if (field.native == "Pilon/glassFrostiness")
        return value.toDouble() == value.toInt() && value.toInt() >= 0 && value.toInt() <= 100;
    if (field.native == "Pilon/colourModeIndex")
        return value.toDouble() == value.toInt() && value.toInt() >= 0 && value.toInt() <= 3;
    if (field.keys.first() == "integrations" || field.native.startsWith("addons/enabled/")) return type == QJsonValue::Bool;
    if (field.native == "assistant/effort" || field.keys.first() == "reasoning")
        return type == QJsonValue::Double && value.toInt() >= 0 && value.toInt() <= 3 && value.toDouble() == value.toInt();
    if (field.native == "calendar/portablePreferences") {
        if (type != QJsonValue::Object) return false;
        const auto options = value.toMap();
        if (options.contains("timeZone") && !QTimeZone(options.value("timeZone").toString().toUtf8()).isValid())
            return false;
        const auto inRange = [&](const QString &key, int lo, int hi) {
            if (!options.contains(key)) return true;
            const auto v = options.value(key);
            return QJsonValue::fromVariant(v).isDouble() && v.toDouble() == v.toInt() && v.toInt() >= lo && v.toInt() <= hi;
        };
        return inRange("firstDay", 1, 7) && inRange("reminder", -1, 10080) &&
               (!options.contains("eventLayout") ||
                QStringList{"columns", "overlap"}.contains(options.value("eventLayout").toString()));
    }
    if (field.native == "notes/portablePreferences") {
        if (type != QJsonValue::Object) return false;
        const auto options = value.toMap();
        for (const auto &flag : {"recognizeShapes", "showPageOutlines", "drawWithFinger", "pressureSensitive"})
            if (options.contains(flag) && !QJsonValue::fromVariant(options.value(flag)).isBool()) return false;
        return (!options.contains("paper") || QJsonValue::fromVariant(options.value("paper")).isObject()) &&
               (!options.contains("pens") || QJsonValue::fromVariant(options.value("pens")).isArray());
    }
    if (field.codec == Codec::JsonText || field.codec == Codec::JsonBytes)
        return type == QJsonValue::Object || type == QJsonValue::Array;
    return true;
}
bool validateFields(const PiconfDocument &doc, const QList<Field> &catalog, QString &error) {
    const auto addons = doc.values().value("addons").toMap();
    for (auto it = addons.cbegin(); it != addons.cend(); ++it) {
        const auto entry = it.value().toMap();
        const auto source = entry.value("source", builtins.contains(it.key()) ? "builtin" : "local").toString();
        const QUrl link(source);
        if (!validId(it.key()) || (source != "local" && source != "builtin" &&
                                   (link.scheme() != "https" || link.host().isEmpty() || !link.isValid() ||
                                    !link.userInfo().isEmpty() || link.hasQuery() || link.hasFragment()))) {
            error = QObject::tr("Invalid add-on ID or acquisition source for %1.").arg(it.key());
            return false;
        }
        if (entry.contains("version") &&
            (entry.value("version").typeId() != QMetaType::QString ||
             entry.value("version").toString().trimmed().isEmpty() || entry.value("version").toString().size() > 64 ||
             entry.value("version").toString().contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))) {
            error = QObject::tr("Invalid package version for %1.").arg(it.key());
            return false;
        }
        if (entry.contains("sha256") &&
            !QRegularExpression(R"(\A[0-9a-fA-F]{64}\z)").match(entry.value("sha256").toString()).hasMatch()) {
            error = QObject::tr("Invalid package checksum for %1.").arg(it.key());
            return false;
        }
    }
    for (const auto &field : catalog) {
        const QVariant value = nested(doc.values(field.file), field.keys);
        if (!supported(field, value)) {
            error = QObject::tr("Invalid value for %1 in %2.").arg(field.keys.join('.'), field.file);
            return false;
        }
    }
    return true;
}

enum class Direction { Startup, Running, Imported };
bool synchronize(PiconfDocument &doc, Stores &stores, Direction direction, bool newWorkspace, Files &updated, QString &error,
                 const QString &onlyAddon = {}) {
    const auto catalog = fields(doc, stores, error);
    if (!error.isEmpty() || !validateFields(doc, catalog, error)) return false;
    QSet<QString> changed;
    if (direction != Direction::Imported && onlyAddon.isEmpty() && !seedLegacyOptions(stores, changed, error)) return false;
    QList<PiconfDocument::Edit> edits;
    for (const auto &field : catalog) {
        if (!onlyAddon.isEmpty() && field.file != doc.addonFile(onlyAddon)) continue;
        const auto store = stores.settings.value(field.store);
        auto raw =
            field.store == hostStore ? store->value(field.native) : PiconfPreferences::scopedValue(*store, field.native);
        if (raw.isValid() && field.keys.first() == "integrations") raw = raw.toBool();
        if (raw.isValid() && field.keys.first() == "reasoning") raw = raw.toInt();
        const auto oldRoot = stores.baseline.value("$workspace", stores.root).toString();
        const bool movedAppearance = field.codec == Codec::Appearance && oldRoot != stores.root;
        const auto native = asPortable(field, raw, movedAppearance ? oldRoot : stores.root);
        const auto key = field.identity();
        const auto previous = stores.baseline.value(key);
        if (field.native.startsWith("addons/enabled/")) {
            const auto id = field.keys.at(1);
            const auto receipt = stores.receipts.value(id).toMap();
            const bool missing = receipt.isEmpty()
                                     ? !builtins.contains(id)
                                     : receipt.value("removed").toBool() ||
                                           !QFileInfo::exists(stores.root + "/addons/" + receipt.value("file").toString());
            if (missing) {
                // Keep the requested state in the declaration. Installation explicitly enables it after dependency checks.
                if (native.toBool()) {
                    store->setValue(field.native, false);
                    changed.insert(field.store);
                }
                const auto requested = nested(doc.values(field.file), field.keys);
                if (!requested.isValid()) edits.append({field.file, field.keys, false});
                stores.baseline.insert(key, requested.isValid() ? requested : QVariant(false));
                stores.baseline.insert("$missing/" + id, true);
                continue;
            }
            if (stores.baseline.contains("$missing/" + id)) {
                const auto requested = nested(doc.values(field.file), field.keys);
                if (direction != Direction::Imported && requested.toBool() && !raw.toBool()) continue;
                stores.baseline.remove("$missing/" + id);
            }
        }
        auto fileValue = nested(doc.values(field.file), field.keys);
        const bool present = fileValue.isValid();
        if (!present) fileValue = field.fallback;
        if (field.codec == Codec::Colour && fileValue.isValid())
            fileValue = QColor(fileValue.toString()).name(QColor::HexArgb);
        QVariant desired = fileValue;
        const bool initialNative = newWorkspace || (direction != Direction::Imported && !previous.isValid() && !present);
        if (initialNative || (direction != Direction::Imported && previous.isValid() && native != previous)) {
            if (!initialNative && fileValue != previous && native != fileValue) {
                error = QObject::tr("Both Settings and %1 changed %2. Restore one value before retrying.")
                            .arg(field.file, field.keys.join('.'));
                return false;
            }
            desired = native;
            if ((!present && desired.isValid()) || desired != fileValue) edits.append({field.file, field.keys, desired});
        } else if (direction == Direction::Running && previous.isValid() && fileValue != previous && native == previous) {
            // External edits take effect when managers are recreated, preserving open drafts.
            continue;
        }
        if (!supported(field, desired)) {
            error = QObject::tr("Cannot convert saved preference %1 in %2.").arg(field.native, field.store);
            return false;
        }
        if (desired != native || (movedAppearance && desired.isValid() && asPortable(field, raw, stores.root) != desired)) {
            if (desired.isValid())
                store->setValue(field.native, asNative(field, desired, raw, stores.root));
            else
                store->remove(field.native);
            if (field.store != hostStore)
                PiconfPreferences::setScopedType(*store, field.native, asNative(field, desired, raw, stores.root));
            if (field.native == "notes/portablePreferences" || field.native == "calendar/portablePreferences")
                store->setValue(field.native + "Pending", true);
            changed.insert(field.store);
        }
        if (desired.isValid())
            stores.baseline.insert(key, desired);
        else
            stores.baseline.remove(key);
    }
    if (!edits.isEmpty() && !doc.apply(edits, {}, error)) return false;
    stores.baseline.insert("$workspace", stores.root);
    return stores.collect(changed, updated, error);
}
} // namespace

QVariant PiconfPreferences::scopedValue(const QSettings &store, const QString &key, const QVariant &fallback) {
    if (!store.contains(key)) return fallback;
    auto value = store.value(key);
    const auto type = store.value("piconf-types/" + key).toInt();
    if (type == QJsonValue::Bool) return value.toBool();
    if (type == QJsonValue::Double) return value.toDouble();
    return value;
}
void PiconfPreferences::setScopedType(QSettings &store, const QString &key, const QVariant &value) {
    const auto type = QJsonValue::fromVariant(value).type();
    const auto metadata = "piconf-types/" + key;
    if (type == QJsonValue::Bool || type == QJsonValue::Double)
        store.setValue(metadata, int(type));
    else
        store.remove(metadata);
}

PiconfPreferences::PiconfPreferences() {
    m_timer.setSingleShot(true);
    m_timer.setInterval(150);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_timer.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_timer.start(); });
    connect(&m_timer, &QTimer::timeout, this, [this] {
        QString error;
        if (!flush(error) && reportError) reportError(error);
    });
}
PiconfPreferences &PiconfPreferences::instance() {
    static PiconfPreferences preferences;
    return preferences;
}
void PiconfPreferences::watch(const QStringList &names) {
    QSet<QString> desired;
    for (const auto &name : names) {
        const auto path = m_root + '/' + name;
        if (QFileInfo::exists(path)) desired.insert(path);
        auto parent = QFileInfo(path).absolutePath();
        while (parent.startsWith(m_root + '/') && !QFileInfo(parent).isDir()) parent = QFileInfo(parent).absolutePath();
        desired.insert(parent);
    }
    const auto watched = m_watcher.files() + m_watcher.directories();
    for (const auto &path : watched)
        if (!desired.remove(path)) m_watcher.removePath(path);
    if (!desired.isEmpty()) m_watcher.addPaths(desired.values());
}
void PiconfPreferences::stop() {
    m_timer.stop();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty()) m_watcher.removePaths(paths);
    m_root.clear();
    m_active = false;
}
bool PiconfPreferences::start(const QString &workspace, QString &error) {
    stop();
    error.clear();
    m_root = QDir::cleanPath(workspace);
    if (!QDir::isAbsolutePath(m_root)) {
        m_root.clear();
        error = tr("The configuration workspace path must be absolute.");
        return false;
    }
    if (!PiconfTransaction::recover(m_root, error)) {
        m_root.clear();
        return false;
    }
    m_active = reconcile(true, error);
    return m_active;
}
bool PiconfPreferences::flush(QString &error) {
    error.clear();
    return !m_active || reconcile(false, error);
}
bool PiconfPreferences::prepareInstalled(const QString &id, QString &error) {
    error.clear();
    if (!validId(id) || !active()) {
        error = tr("Open a valid workspace configuration before restoring an add-on.");
        return false;
    }
    PiconfDocument doc;
    Stores stores{m_root};
    if (!doc.load(m_root, error) || !stores.initialize(error)) return false;
    if (doc.addonFile(id).isEmpty()) {
        error = tr("The configuration does not declare %1.").arg(id);
        return false;
    }
    auto expected = doc.files();
    Files updated;
    if (!synchronize(doc, stores, Direction::Running, false, updated, error, id)) return false;
    for (auto it = doc.files().cbegin(); it != doc.files().cend(); ++it)
        if (expected.value(it.key()) != it.value()) updated[it.key()] = it.value();
    expected.insert(stores.before);
    return updated.isEmpty() || PiconfTransaction::commit(m_root, expected, updated, error);
}
bool PiconfPreferences::validate(const PiconfDocument &document, QString &error) const {
    error.clear();
    Stores stores{m_root};
    if (m_root.isEmpty()) {
        error = tr("Open a workspace before importing a configuration.");
        return false;
    }
    if (!stores.initialize(error)) return false;
    const auto catalog = fields(document, stores, error);
    return error.isEmpty() && validateFields(document, catalog, error);
}
bool PiconfPreferences::prepareImported(const PiconfDocument &document, Files &expected, Files &updated,
                                        QString &error) const {
    error.clear();
    Stores stores{m_root};
    if (m_root.isEmpty()) {
        error = tr("Open a workspace before importing a configuration.");
        return false;
    }
    if (!stores.initialize(error)) return false;
    auto doc = document;
    if (!synchronize(doc, stores, Direction::Imported, false, updated, error)) return false;
    expected.insert(stores.before);
    return true;
}
bool PiconfPreferences::reconcile(bool starting, QString &error) {
    error.clear();
    Stores stores{m_root};
    if (!stores.initialize(error)) return false;
    PiconfDocument doc;
    const bool creating = !QFileInfo::exists(m_root + '/' + PiconfDocument::MainPath);
    if (creating && !stores.baseline.isEmpty()) {
        error = tr("The main configuration is missing. Restore config/pilon.piconf before continuing.");
        return false;
    }
    if (creating) {
        if (!doc.parse({{PiconfDocument::MainPath, PiconfDocument::encode({{"format", 1}})}}, error)) return false;
    } else if (!doc.load(m_root, error))
        return false;
    auto original = creating ? Files{} : doc.files();
    const auto existing = doc.values().value("addons").toMap();
    if (!ensureDeclarations(doc, stores, error)) return false;
    for (const auto &id : doc.values().value("addons").toMap().keys()) {
        if (!existing.contains(id) && !readExisting(m_root, doc.addonFile(id), original, error)) return false;
    }
    Files updated;
    if (!synchronize(doc, stores, starting ? Direction::Startup : Direction::Running, creating, updated, error))
        return false;
    for (auto it = doc.files().cbegin(); it != doc.files().cend(); ++it)
        if (!original.contains(it.key()) || original[it.key()] != it.value()) updated[it.key()] = it.value();
    original.insert(stores.before);
    if (!updated.isEmpty() && !PiconfTransaction::commit(m_root, original, updated, error)) return false;
    watch(stores.settings.keys() + QStringList{receiptsFile});
    return true;
}
