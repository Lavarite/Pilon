#include "addonregistry.h"
#include "../workspace/pilonsettings.h"
#include "../workspace/pilonworkspace.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QResource>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QUuid>
#include <QVersionNumber>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

namespace {
const QStringList builtinIds{"files", "ssh", "assistant", "notes", "calendar", "git", "devices"};
QString enabledKey(const QString &id) {
    return "addons/enabled/" + id;
}
QString integrationKey(const QString &owner, const QString &id) {
    return "addons/integrations/" + owner + '/' + id;
}
QString issue(const char *text) {
    return QCoreApplication::translate("AddonRegistry", text);
}
bool matches(const QString &text, const char *pattern) {
    return QRegularExpression(QString::fromLatin1(pattern)).match(text).hasMatch();
}
bool sectionId(const QString &id) {
    return matches(id, "\\A[a-z][a-z0-9-]{0,63}\\z");
}
bool packageId(const QString &id) {
    return id.size() <= 128 && matches(id, "\\A[a-z][a-z0-9-]*(?:\\.[a-z][a-z0-9-]*)+\\z");
}
bool addonId(const QString &id) {
    return builtinIds.contains(id) || packageId(id);
}
QVersionNumber version(const QString &value) {
    if (!matches(value, "\\A(?:0|[1-9][0-9]*)\\.(?:0|[1-9][0-9]*)\\.(?:0|[1-9][0-9]*)\\z"))
        return {};
    QList<int> parts;
    for (const auto &part : value.split('.')) {
        bool ok = false;
        const auto number = part.toInt(&ok);
        if (!ok)
            return {};
        parts << number;
    }
    return QVersionNumber(parts);
}
bool text(const QJsonValue &value, int limit = 128) {
    if (!value.isString())
        return false;
    const auto s = value.toString();
    return !s.trimmed().isEmpty() && s.size() <= limit &&
           !s.contains(QRegularExpression("[\\x00-\\x1f\\x7f]"));
}
bool localPath(const QString &path) {
    if (path.isEmpty() || path.startsWith('/') || path.contains('\\') || path.contains(':') ||
        path.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))
        return false;
    for (const auto &part : path.split('/'))
        if (part.isEmpty() || part == "." || part == "..")
            return false;
    return true;
}
bool privateDirectory(const QString &path, QString *error) {
    const auto clean = QDir::cleanPath(path);
    for (QString part = clean; part != "/"; part = QFileInfo(part).absolutePath()) {
        if (QFileInfo(part).isSymLink()) {
            *error = issue("Add-on storage cannot use symbolic links.");
            return false;
        }
    }
    const bool existed = QFileInfo::exists(clean);
    if ((!existed && !QDir().mkpath(clean)) || !QFileInfo(clean).isDir()) {
        *error = issue("Cannot create the add-on storage folder.");
        return false;
    }
    const auto owner = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    const auto permissions = existed ? QFileInfo(clean).permissions() & owner : owner;
    if (!QFile::setPermissions(clean, permissions)) {
        *error = issue("Cannot make add-on storage private.");
        return false;
    }
    return true;
}
QByteArray fileHash(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? hash.result() : QByteArray{};
}
bool copyPackage(const QString &source, const QString &destination, QString *error) {
    if (!QFileInfo(source).isFile() || QFileInfo(source).isSymLink() || QFileInfo(destination).isSymLink()) {
        *error = issue("Packages must be regular files, not symbolic links.");
        return false;
    }
    QFile input(source);
    QSaveFile output(destination);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        *error = issue("Cannot copy the add-on package.");
        return false;
    }
    output.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    while (!input.atEnd()) {
        const auto bytes = input.read(1024 * 1024);
        if ((bytes.isEmpty() && input.error() != QFile::NoError) || output.write(bytes) != bytes.size()) {
            *error = issue("Cannot copy the add-on package.");
            return false;
        }
    }
    if (!output.commit()) {
        *error = issue("Cannot save the add-on package.");
        return false;
    }
    return true;
}
// Mounts outlive QML engines, including a reset's engine teardown/recreation.
class ResourcePool : public QObject {
  public:
    using QObject::QObject;
    ~ResourcePool() override {
        for (const auto &mount : mounts)
            QResource::unregisterResource(mount.first, mount.second);
    }
    QHash<QString, QPair<QString, QString>> mounts;
};
ResourcePool *pool() {
    static auto value = new ResourcePool(QCoreApplication::instance());
    return value;
}
QUrl resourceUrl(const QString &root, const QString &path) {
    QUrl url;
    url.setScheme("qrc");
    url.setPath(root + '/' + path);
    return url;
}
QVariantMap readPackage(const QString &path, bool builtin, const QString &expectedId, const QByteArray &hash,
                        bool retain, const QString &cacheDirectory = {}) {
    QVariantMap row{{"id", expectedId},
                    {"name", QFileInfo(path).completeBaseName()},
                    {"description", ""},
                    {"version", ""},
                    {"builtin", builtin},
                    {"page", QUrl{}},
                    {"settings", QVariantList{}},
                    {"runtime", QUrl{}},
                    {"widgets", QVariantList{}},
                    {"startOnEnable", false},
                    {"icon", QUrl{}},
                    {"lifecycle", QUrl{}},
                    {"dependencies", QVariantList{}},
                    {"extensionPoints", QVariantList{}},
                    {"integrations", QVariantList{}},
                    {"error", ""}};
    const auto reject = [&](const QString &message) {
        row["error"] = message;
        return row;
    };
    const QFileInfo file(path);
    if (!file.exists() || !file.isFile() || file.isSymLink())
        return reject(issue("The .piext package is missing or is a symbolic link."));
    if (file.suffix().compare("piext", Qt::CaseInsensitive) != 0 || hash.isEmpty())
        return reject(issue("Choose a readable .piext package."));
    QString mountedPath = path;
    if (retain) {
        QString problem;
        if (!privateDirectory(cacheDirectory, &problem))
            return reject(problem);
        mountedPath = QDir(cacheDirectory).absoluteFilePath(QString::fromLatin1(hash.toHex()) + ".piext");
        if (!QFileInfo::exists(mountedPath) && !copyPackage(path, mountedPath, &problem))
            return reject(problem);
        if (QFileInfo(mountedPath).isSymLink() || fileHash(mountedPath) != hash)
            return reject(
                issue("The add-on changed while it was being copied. Try again after the copy finishes."));
    }
    const QString cacheKey =
        QFileInfo(mountedPath).absoluteFilePath() + ':' + QString::fromLatin1(hash.toHex());
    const auto existing = pool()->mounts.value(cacheKey);
    const auto root = existing.second.isEmpty()
                          ? "/pilon-addons/" + QUuid::createUuid().toString(QUuid::Id128)
                          : existing.second;
    const bool newlyMounted = existing.second.isEmpty();
    if (newlyMounted && !QResource::registerResource(mountedPath, root))
        return reject(issue("This file is not a valid Qt resource package."));
    bool accepted = false;
    const auto result = [&]() -> QVariantMap {
        const auto manifestPath = ':' + root + "/manifest.json";
        QResource resource(manifestPath);
        if (!resource.isValid() || resource.uncompressedSize() > 65536)
            return reject(issue("The package needs a manifest.json of at most 64 KiB at its root."));
        QFile manifest(manifestPath);
        if (!manifest.open(QIODevice::ReadOnly))
            return reject(issue("Cannot read manifest.json."));
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(manifest.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject())
            return reject(issue("manifest.json must be a JSON object."));
        const auto data = document.object();
        const QStringList keys{
            "id",           "name",     "description",   "version", "apiVersion",   "page",
            "settings",     "runtime",  "startOnEnable", "icon",    "dependencies", "extensionPoints",
            "integrations", "lifecycle", "widgets"};
        for (auto i = data.begin(); i != data.end(); ++i)
            if (!keys.contains(i.key()))
                return reject(issue("Unknown manifest field: ") + i.key());
        if (!data.value("apiVersion").isDouble() || data.value("apiVersion").toDouble() != 1)
            return reject(issue("This package requires an unsupported add-on API version."));
        const auto id = data.value("id").toString();
        if (!addonId(id) || (builtin && (!builtinIds.contains(id) || id != expectedId)))
            return reject(issue("Use a unique namespaced add-on ID or a known shipped ID."));
        row["id"] = id;
        if (!text(data.value("name")) || !text(data.value("version"), 64))
            return reject(issue("The manifest needs a name and version."));
        row["name"] = data.value("name").toString();
        row["version"] = data.value("version").toString();
        if (data.contains("description") && (!text(data.value("description"), 400) ||
                                             data.value("description").toString().toUcs4().size() > 200))
            return reject(issue("The description must be nonblank text of at most 200 characters without "
                                "control characters."));
        row["description"] = data.value("description").toString();
        const auto entry = [&](const QJsonValue &value, bool qml) -> QUrl {
            if (!value.isString() || !localPath(value.toString()) ||
                (qml && !value.toString().endsWith(".qml")))
                return {};
            if (!QFileInfo(':' + root + '/' + value.toString()).isFile())
                return {};
            return resourceUrl(root, value.toString());
        };
        if (data.contains("page")) {
            const auto page = entry(data.value("page"), true);
            if (page.isEmpty())
                return reject(issue("The page must name a QML file inside the package."));
            row["page"] = page;
        }
        if (data.contains("runtime")) {
            const auto runtime = entry(data.value("runtime"), true);
            if (runtime.isEmpty())
                return reject(issue("The runtime must name a QML file inside the package."));
            row["runtime"] = runtime;
        }
        if (data.contains("startOnEnable") && !data.value("startOnEnable").isBool())
            return reject(issue("startOnEnable must be a boolean."));
        if (data.value("startOnEnable").toBool() && row.value("runtime").toUrl().isEmpty())
            return reject(issue("startOnEnable requires a runtime."));
        row["startOnEnable"] = data.value("startOnEnable").toBool();
        if (data.contains("widgets") &&
            (!data.value("widgets").isArray() || data.value("widgets").toArray().size() > 16))
            return reject(issue("widgets must be an array of at most 16 entries."));
        const auto widgetDeclarations = data.value("widgets").toArray();
        if (!widgetDeclarations.isEmpty() && row.value("runtime").toUrl().isEmpty())
            return reject(issue("Widget declarations require a runtime."));
        QVariantList widgets;
        QStringList widgetIds;
        for (const auto &value : widgetDeclarations) {
            const auto widget = value.toObject();
            const auto widgetId = widget.value("id").toString();
            if (!value.isObject() || widget.size() != 2 || !widget.contains("id") || !widget.contains("name"))
                return reject(issue("Each widget needs exactly id and name."));
            if (!sectionId(widgetId) || widgetIds.contains(widgetId) || !text(widget.value("name"), 256) ||
                widget.value("name").toString().toUcs4().size() > 128)
                return reject(issue("Widgets need unique IDs and nonblank names of at most 128 characters without control characters."));
            widgetIds << widgetId;
            widgets << widget.toVariantMap();
        }
        row["widgets"] = widgets;
        if (data.contains("icon")) {
            const auto icon = entry(data.value("icon"), false);
            if (icon.isEmpty())
                return reject(issue("The icon must name a file inside the package."));
            row["icon"] = icon;
        }
        QVariantList settings;
        QStringList ids;
        if (data.contains("settings") &&
            (!data.value("settings").isArray() || data.value("settings").toArray().size() > 32))
            return reject(issue("settings must be an array of at most 32 sections."));
        for (const auto &value : data.value("settings").toArray()) {
            if (!value.isObject())
                return reject(issue("Each settings section needs an id, name and QML entry."));
            const auto section = value.toObject();
            for (auto i = section.begin(); i != section.end(); ++i)
                if (!QStringList{"id", "name", "entry"}.contains(i.key()))
                    return reject(issue("Unknown settings field: ") + i.key());
            const auto sectionName = section.value("id").toString();
            const auto url = entry(section.value("entry"), true);
            if (!sectionId(sectionName) || ids.contains(sectionName) || !text(section.value("name")) ||
                url.isEmpty())
                return reject(
                    issue("Settings sections need unique IDs, names and package-local QML entries."));
            ids << sectionName;
            settings.append(
                QVariantMap{{"id", sectionName}, {"name", section.value("name").toString()}, {"entry", url}});
        }
        row["settings"] = settings;
        if (data.contains("lifecycle")) {
            const auto lifecycle = entry(data.value("lifecycle"), true);
            if (lifecycle.isEmpty())
                return reject(issue("The lifecycle must name a QML file inside the package."));
            row["lifecycle"] = lifecycle;
        }
        const auto records = [&](const QString &key) {
            return !data.contains(key) ||
                   (data.value(key).isArray() && data.value(key).toArray().size() <= 32);
        };
        for (const auto &key : {QString("dependencies"), QString("extensionPoints"), QString("integrations")})
            if (!records(key))
                return reject(key + issue(" must be an array of at most 32 entries."));
        QVariantList dependencies;
        ids.clear();
        for (const auto &value : data.value("dependencies").toArray()) {
            if (!value.isObject())
                return reject(issue("Dependencies need id, minVersion and optional maxVersion."));
            const auto dependency = value.toObject();
            for (auto i = dependency.begin(); i != dependency.end(); ++i)
                if (!QStringList{"id", "minVersion", "maxVersion"}.contains(i.key()))
                    return reject(issue("Unknown dependency field: ") + i.key());
            const auto dependencyId = dependency.value("id").toString();
            const auto minimum = version(dependency.value("minVersion").toString());
            const auto maximum = version(dependency.value("maxVersion").toString());
            if (!addonId(dependencyId) || dependencyId == id || ids.contains(dependencyId) ||
                minimum.isNull() ||
                (dependency.contains("maxVersion") && (maximum.isNull() || maximum <= minimum)))
                return reject(issue("Dependencies need unique other IDs and numeric inclusive "
                                    "minimum/exclusive maximum versions."));
            ids << dependencyId;
            dependencies << dependency.toVariantMap();
        }
        row["dependencies"] = dependencies;
        QVariantList points;
        ids.clear();
        const auto api = [](const QJsonValue &value) {
            return value.isDouble() && value.toDouble() >= 1 && value.toDouble() <= INT_MAX &&
                   value.toDouble() == value.toInt();
        };
        for (const auto &value : data.value("extensionPoints").toArray()) {
            const auto point = value.toObject();
            const auto pointId = point.value("id").toString();
            if (!value.isObject() || point.size() != 2 || !point.contains("apiVersion") ||
                !sectionId(pointId) || ids.contains(pointId) || !api(point.value("apiVersion")))
                return reject(issue("Extension points need unique IDs and a positive integer apiVersion."));
            ids << pointId;
            points << point.toVariantMap();
        }
        row["extensionPoints"] = points;
        QVariantList integrations;
        ids.clear();
        const QStringList fields{"id", "name", "target", "minVersion", "point", "apiVersion", "entry"};
        for (const auto &value : data.value("integrations").toArray()) {
            auto integration = value.toObject();
            const auto integrationId = integration.value("id").toString();
            if (!value.isObject() || integration.size() < fields.size())
                return reject(
                    issue("Integrations need id, name, target, minVersion, point, apiVersion and entry."));
            for (auto i = integration.begin(); i != integration.end(); ++i)
                if (!fields.contains(i.key()) && i.key() != "enabledByDefault")
                    return reject(issue("Unknown integration field: ") + i.key());
            if (integration.contains("enabledByDefault") && !integration.value("enabledByDefault").isBool())
                return reject(issue("Integration enabledByDefault must be a boolean."));
            const auto target = integration.value("target").toString();
            const auto url = entry(integration.value("entry"), true);
            if (!sectionId(integrationId) || ids.contains(integrationId) ||
                !text(integration.value("name")) || !addonId(target) || target == id ||
                !sectionId(integration.value("point").toString()) || !api(integration.value("apiVersion")) ||
                version(integration.value("minVersion").toString()).isNull() || url.isEmpty())
                return reject(issue("Integrations need unique IDs, names, another target, numeric minimum, "
                                    "point API and a package-local entry."));
            ids << integrationId;
            auto descriptor = integration.toVariantMap();
            descriptor["entry"] = url;
            integrations << descriptor;
        }
        row["integrations"] = integrations;
        if (row.value("page").toUrl().isEmpty() && row.value("runtime").toUrl().isEmpty() &&
            settings.isEmpty() && integrations.isEmpty() && row.value("lifecycle").toUrl().isEmpty())
            return reject(
                issue("The add-on needs a page, runtime, settings, integration or lifecycle entry."));
        accepted = true;
        return row;
    }();
    if (newlyMounted) {
        if (accepted && retain)
            pool()->mounts[cacheKey] = {mountedPath, root};
        else
            QResource::unregisterResource(mountedPath, root);
    }
    return result;
}
bool validKey(const QString &key) {
    return matches(key, "\\A[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\\z");
}
bool preferenceValue(const QVariant &value, int depth = 0) {
    if (depth > 32)
        return false;
    if (!value.isValid() || value.isNull())
        return true;
    switch (value.metaType().id()) {
    case QMetaType::Bool:
    case QMetaType::QString:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::QStringList:
        return true;
    case QMetaType::Double:
        return std::isfinite(value.toDouble());
    case QMetaType::QVariantList:
        for (const auto &item : value.toList())
            if (!preferenceValue(item, depth + 1))
                return false;
        return true;
    case QMetaType::QVariantMap:
        for (const auto &item : value.toMap())
            if (!preferenceValue(item, depth + 1))
                return false;
        return true;
    default:
        return false;
    }
}
bool ownedPaths(const QVariantMap &input, QVariantMap *normalized) {
    for (auto i = input.cbegin(); i != input.cend(); ++i) {
        if (!QStringList{"personal", "downloads", "addonData"}.contains(i.key()) ||
            (i->metaType().id() != QMetaType::QVariantList && i->metaType().id() != QMetaType::QStringList))
            return false;
        const auto values = i->toList();
        if (values.size() > 128)
            return false;
        QVariantList paths;
        for (const auto &value : values) {
            if (value.metaType().id() != QMetaType::QString || value.toString().size() > 4096 ||
                !localPath(value.toString()))
                return false;
            if (!paths.contains(value))
                paths << value;
        }
        (*normalized)[i.key()] = paths;
    }
    return true;
}
} // namespace

AddonRegistry::AddonRegistry(QObject *parent)
    : AddonRegistry(
#ifdef Q_OS_ANDROID
                    QStringLiteral(":/pilon-addons"),
#else
                    QCoreApplication::applicationDirPath() + "/addons",
#endif
                    PilonWorkspace::addonsDirectory(),
                    PilonWorkspace::addonDataRoot(), PilonWorkspace::appDataDirectory() + "/addons",
                    QCoreApplication::arguments().contains("--disable-addons"), parent) {}
AddonRegistry::AddonRegistry(const QString &builtins, const QString &installed, const QString &data,
                             bool recovery, QObject *parent)
    : AddonRegistry(builtins, installed, data, data + "/.registry", recovery, parent) {}
AddonRegistry::AddonRegistry(const QString &builtins, const QString &installed, const QString &data,
                             const QString &state, bool recovery, QObject *parent)
    : QObject(parent), m_builtins(builtins), m_installed(installed), m_data(data), m_recovery(recovery),
      m_stateDirectory(QDir::cleanPath(state)) {
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(150);
    connect(&m_refreshTimer, &QTimer::timeout, this, &AddonRegistry::refresh);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_refreshTimer.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_refreshTimer.start(); });
    refresh();
}
bool AddonRegistry::loadState() {
    if (m_stateLoaded)
        return true;
    QString problem;
    if (!privateDirectory(m_stateDirectory, &problem))
        return fail(problem);
    const auto path = m_stateDirectory + "/registry.json";
    if (!QFileInfo::exists(path)) {
        m_stateLoaded = true;
        return true;
    }
    QFile file(path);
    if (QFileInfo(path).isSymLink() || file.size() > 1024 * 1024 || !file.open(QIODevice::ReadOnly))
        return fail(issue("Cannot read the add-on ownership records."));
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    const auto state = document.object();
    if (parse.error != QJsonParseError::NoError || !document.isObject() ||
        state.value("schema").toInt() != 1 || !state.value("seeded").isArray() ||
        !state.value("owners").isObject())
        return fail(
            issue("The add-on ownership records are invalid. Restore the records before changing add-ons."));
    QStringList seeded;
    QVariantMap receipts;
    for (const auto &value : state.value("seeded").toArray()) {
        if (!value.isString() || !builtinIds.contains(value.toString()))
            return fail(issue("The add-on seed records are invalid."));
        seeded << value.toString();
    }
    const auto owners = state.value("owners").toObject();
    for (auto i = owners.begin(); i != owners.end(); ++i) {
        const auto owner = i->toObject();
        if (!addonId(i.key()) || !i->isObject() || owner.value("id").toString() != i.key() ||
            !text(owner.value("name")) || !owner.value("lifecycleDeclared").isBool())
            return fail(issue("The add-on ownership records are invalid."));
        QVariantMap paths;
        if (owner.contains("ownedPaths") &&
            (!owner.value("ownedPaths").isObject() ||
             !ownedPaths(owner.value("ownedPaths").toObject().toVariantMap(), &paths)))
            return fail(issue("The add-on ownership paths are invalid."));
        receipts[i.key()] = owner.toVariantMap();
    }
    QVariantMap seedHashes;
    if (state.contains("seedHashes") && !state.value("seedHashes").isObject())
        return fail(issue("The add-on seed hashes are invalid."));
    const auto hashes = state.value("seedHashes").toObject();
    for (auto i = hashes.begin(); i != hashes.end(); ++i) {
        if (!builtinIds.contains(i.key()) || !i->isString() || !matches(i->toString(), "\\A[0-9a-f]{64}\\z"))
            return fail(issue("The add-on seed hashes are invalid."));
        seedHashes[i.key()] = i->toString();
    }
    m_seeded = seeded;
    m_seedHashes = seedHashes;
    m_receipts = receipts;
    m_stateLoaded = true;
    return true;
}
bool AddonRegistry::saveState() {
    QString problem;
    if (!privateDirectory(m_stateDirectory, &problem))
        return fail(problem);
    const auto path = m_stateDirectory + "/registry.json";
    if (QFileInfo(path).isSymLink())
        return fail(issue("Add-on ownership records cannot use symbolic links."));
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly))
        return fail(issue("Cannot save add-on ownership records."));
    output.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    const auto bytes = QJsonDocument(QJsonObject{{"schema", 1},
                                                 {"seeded", QJsonArray::fromStringList(m_seeded)},
                                                 {"seedHashes", QJsonObject::fromVariantMap(m_seedHashes)},
                                                 {"owners", QJsonObject::fromVariantMap(m_receipts)}})
                           .toJson();
    if (bytes.size() > 1024 * 1024)
        return fail(issue("The add-on ownership records are too large."));
    if (output.write(bytes) != bytes.size() || !output.commit())
        return fail(issue("Cannot save add-on ownership records."));
    return true;
}
bool AddonRegistry::seedPackages() {
    QString problem;
    if (!privateDirectory(m_installed, &problem))
        return fail(problem);
    const auto oldSeeded = m_seeded;
    const auto oldHashes = m_seedHashes;
    QStringList created;
    std::unique_ptr<QTemporaryDir> backups;
    QMap<QString, QString> replaced;
    bool complete = false;
    const auto rollback = qScopeGuard([&] {
        if (complete)
            return;
        for (const auto &path : created)
            QFile::remove(path);
        for (auto i = replaced.cbegin(); i != replaced.cend(); ++i) {
            QString restoreError;
            if (!copyPackage(i.value(), i.key(), &restoreError)) {
                backups->setAutoRemove(false);
                m_error += issue(" Restore the package backup before retrying: ") + i.value();
            }
        }
        m_seeded = oldSeeded;
        m_seedHashes = oldHashes;
    });
    for (const auto &id : builtinIds) {
        const auto source = QDir(m_builtins).absoluteFilePath(id + ".piext");
        if (!QFileInfo::exists(source))
            continue;
        const auto sourceHash = fileHash(source);
        const auto sourceHex = QString::fromLatin1(sourceHash.toHex());
        const auto row = readPackage(source, true, id, sourceHash, source.startsWith(":/"),
                                     m_stateDirectory + "/builtin-packages");
        if (!row.value("error").toString().isEmpty())
            return fail(row.value("error").toString());
        const auto destination = QDir(m_installed).absoluteFilePath(id + ".piext");
        if (m_seeded.contains(id)) {
            if (!QFileInfo::exists(destination) || !m_seedHashes.contains(id) ||
                m_seedHashes.value(id) == sourceHex)
                continue;
            if (QString::fromLatin1(fileHash(destination).toHex()) != m_seedHashes.value(id).toString())
                continue;
            if (!backups) {
                backups = std::make_unique<QTemporaryDir>(QDir(m_installed).absoluteFilePath(".seed-XXXXXX"));
                if (!backups->isValid())
                    return fail(issue("Cannot stage the add-on upgrade."));
            }
            const auto backup = backups->filePath(id + ".piext");
            if (!copyPackage(destination, backup, &problem))
                return fail(problem);
            replaced[destination] = backup;
            if (!copyPackage(source, destination, &problem))
                return fail(problem);
            m_seedHashes[id] = sourceHex;
            continue;
        }
        if (!QFileInfo::exists(destination)) {
            if (!copyPackage(source, destination, &problem))
                return fail(problem);
            created << destination;
        }
        if (fileHash(destination) == sourceHash)
            m_seedHashes[id] = sourceHex;
        m_seeded << id;
    }
    if ((m_seeded != oldSeeded || m_seedHashes != oldHashes) && !saveState())
        return false;
    complete = true;
    return true;
}
void AddonRegistry::rearmWatches() {
    const auto files = m_watcher.files(), directories = m_watcher.directories();
    if (!files.isEmpty())
        m_watcher.removePaths(files);
    if (!directories.isEmpty())
        m_watcher.removePaths(directories);
    QString ancestor = QFileInfo(m_installed).absolutePath();
    while (!QFileInfo(ancestor).isDir() && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList paths{ancestor};
    if (QFileInfo(m_installed).isDir() && !QFileInfo(m_installed).isSymLink())
        paths << m_installed;
    if (!QFileInfo(m_installed).isSymLink())
        for (const auto &file : QDir(m_installed).entryInfoList({"*.piext"}, QDir::Files | QDir::System))
            if (!file.isSymLink())
                paths << file.absoluteFilePath();
    paths.removeDuplicates();
    if (!paths.isEmpty())
        m_watcher.addPaths(paths);
}
bool AddonRegistry::fail(const QString &message) {
    if (m_error != message) {
        m_error = message;
        emit changed();
    }
    return false;
}
const QVariantMap *AddonRegistry::find(const QString &id) const {
    for (const auto &row : m_rows)
        if (row.value("id") == id && row.value("error").toString().isEmpty())
            return &row;
    return nullptr;
}
bool AddonRegistry::configured(const QVariantMap &row) const {
    const auto id = row.value("id").toString();
    return PilonSettings().value(enabledKey(id), row.value("defaultEnabled", false)).toBool();
}
QStringList AddonRegistry::dependencyOrder(const QString &id, QString *error) const {
    QStringList ordered, visiting;
    std::function<bool(const QString &)> visit = [&](const QString &current) {
        if (ordered.contains(current))
            return true;
        if (visiting.contains(current)) {
            *error = issue("Dependency cycle: ") + (visiting + QStringList{current}).join(" -> ");
            return false;
        }
        const auto row = find(current);
        if (!row) {
            *error = issue("Missing or invalid required add-on: ") + current;
            return false;
        }
        if (visiting.size() >= 256) {
            *error = issue("The add-on dependency chain is too deep.");
            return false;
        }
        visiting << current;
        for (const auto &value : row->value("dependencies").toList()) {
            const auto dependency = value.toMap();
            const auto providerId = dependency.value("id").toString();
            const auto provider = find(providerId);
            if (!provider) {
                *error = current + issue(" requires missing or invalid add-on ") + providerId;
                return false;
            }
            const auto actual = version(provider->value("version").toString());
            const auto minimum = version(dependency.value("minVersion").toString());
            const auto maximum = version(dependency.value("maxVersion").toString());
            if (actual.isNull() || actual < minimum || (!maximum.isNull() && actual >= maximum)) {
                *error = current + issue(" requires ") + providerId +
                         " >= " + dependency.value("minVersion").toString();
                if (!maximum.isNull())
                    *error += " < " + dependency.value("maxVersion").toString();
                *error += issue("; installed version: ") + provider->value("version").toString();
                return false;
            }
            if (!visit(providerId))
                return false;
        }
        visiting.removeLast();
        ordered << current;
        return true;
    };
    return visit(id) ? ordered : QStringList{};
}
QStringList AddonRegistry::requiredOrder(const QString &id) {
    QString problem;
    const auto order = dependencyOrder(id, &problem);
    if (!problem.isEmpty())
        fail(problem);
    return order;
}
QStringList AddonRegistry::dependents(const QString &id) const {
    QHash<QString, QStringList> consumers;
    for (const auto &row : m_rows) {
        if (!row.value("error").toString().isEmpty())
            continue;
        const auto consumer = row.value("id").toString();
        for (const auto &value : row.value("dependencies").toList())
            consumers[value.toMap().value("id").toString()] << consumer;
    }
    QStringList result;
    QSet<QString> visited{id};
    std::function<void(const QString &)> visit = [&](const QString &provider) {
        for (const auto &consumer : consumers.value(provider)) {
            if (visited.contains(consumer))
                continue;
            visited.insert(consumer);
            visit(consumer);
            const auto row = find(consumer);
            if (row && configured(*row))
                result << consumer;
        }
    };
    visit(id);
    return result;
}
QVariantList AddonRegistry::addons() const {
    QVariantList rows;
    for (auto row : m_rows) {
        const bool selected = configured(row);
        QString blocked;
        const auto order = dependencyOrder(row.value("id").toString(), &blocked);
        if (blocked.isEmpty() && selected)
            for (const auto &id : order) {
                const auto dependency = find(id);
                if (dependency && !configured(*dependency)) {
                    blocked = issue("Enable required add-on: ") + id;
                    break;
                }
            }
        row["configuredEnabled"] = selected;
        row["blockedReason"] = blocked;
        row["enabled"] =
            !m_recovery && row.value("error").toString().isEmpty() && blocked.isEmpty() && selected;
        rows.append(row);
    }
    return rows;
}
QVariantList AddonRegistry::sidebarAddons() const {
    QVariantList pages;
    const auto hidden = PilonSettings().value("addons/hiddenPages").toStringList();
    for (const auto &value : addons()) {
        auto row = value.toMap();
        if (row.value("error").toString().isEmpty() && !row.value("page").toUrl().isEmpty()) {
            row["sidebarVisible"] = !hidden.contains(row.value("id").toString());
            pages << row;
        }
    }
    const auto order = PilonSettings().value("addons/sidebarOrder").toStringList();
    std::stable_sort(pages.begin(), pages.end(), [&](const auto &left, const auto &right) {
        const auto position = [&](const QVariant &value) {
            const auto index = order.indexOf(value.toMap().value("id").toString());
            return index < 0 ? order.size() : index;
        };
        return position(left) < position(right);
    });
    return pages;
}
bool AddonRegistry::setSidebarOrder(const QStringList &ids) {
    QStringList available;
    for (const auto &value : sidebarAddons())
        available << value.toMap().value("id").toString();
    QStringList order;
    for (const auto &id : ids) {
        if (!available.contains(id) || order.contains(id))
            return fail(issue("The sidebar tabs changed. Reopen the tab order window and try again."));
        order << id;
    }
    for (const auto &id : available)
        if (!order.contains(id))
            order << id;
    PilonSettings settings;
    const auto previous = settings.value("addons/sidebarOrder");
    settings.setValue("addons/sidebarOrder", order);
    settings.sync();
    if (settings.status() != PilonSettings::NoError) {
        if (previous.isValid())
            settings.setValue("addons/sidebarOrder", previous);
        else
            settings.remove("addons/sidebarOrder");
        settings.sync();
        return fail(issue("Cannot save the sidebar tab order. Check your settings folder permissions."));
    }
    m_error.clear();
    emit changed();
    return true;
}
bool AddonRegistry::setPageVisible(const QString &id, bool visible) {
    const auto page = find(id);
    if (!page || page->value("page").toUrl().isEmpty())
        return fail(issue("This add-on has no available page."));
    PilonSettings settings;
    const auto previous = settings.value("addons/hiddenPages");
    auto hidden = previous.toStringList();
    hidden.removeAll(id);
    if (!visible)
        hidden << id;
    settings.setValue("addons/hiddenPages", hidden);
    settings.sync();
    if (settings.status() != PilonSettings::NoError) {
        if (previous.isValid())
            settings.setValue("addons/hiddenPages", previous);
        else
            settings.remove("addons/hiddenPages");
        settings.sync();
        return fail(issue("Cannot save the page visibility. Check your settings folder permissions."));
    }
    m_error.clear();
    emit changed();
    return true;
}
bool AddonRegistry::isEnabled(const QString &id) const {
    const auto row = find(id);
    if (!row || m_recovery || !configured(*row))
        return false;
    QString problem;
    const auto order = dependencyOrder(id, &problem);
    return problem.isEmpty() && std::all_of(order.cbegin(), order.cend(), [&](const auto &dependency) {
               return configured(*find(dependency));
           });
}
bool AddonRegistry::setEnabled(const QString &id, bool enabled) {
    return changeEnabled(id, enabled, {});
}
bool AddonRegistry::disableWithDependents(const QString &id, const QStringList &confirmedDependents) {
    return changeEnabled(id, false, confirmedDependents);
}
bool AddonRegistry::changeEnabled(const QString &id, bool enabled, const QStringList &confirmedDependents) {
    const bool known =
        (builtinIds.contains(id) || packageId(id)) &&
        std::any_of(m_rows.cbegin(), m_rows.cend(), [&](const auto &row) { return row.value("id") == id; });
    if ((!enabled && !known) || (enabled && !find(id)))
        return fail(issue("This add-on is unavailable. Refresh or repair its package first."));
    QString problem;
    auto order = enabled ? dependencyOrder(id, &problem) : QStringList{id};
    if (!problem.isEmpty())
        return fail(problem);
    if (!enabled) {
        const auto requiring = dependents(id);
        if (requiring != confirmedDependents) {
            if (!confirmedDependents.isEmpty())
                return fail(issue("The dependent add-ons changed. Review them and try again."));
            return fail(issue("Disable these required dependents first: ") + requiring.join(", "));
        }
        order = requiring + order;
    }
    PilonSettings settings;
    QHash<QString, QVariant> previous;
    for (const auto &member : order) {
        const auto key = enabledKey(member);
        previous[key] = settings.value(key);
        settings.setValue(key, enabled);
    }
    settings.sync();
    if (settings.status() != PilonSettings::NoError) {
        for (auto i = previous.cbegin(); i != previous.cend(); ++i) {
            if (i->isValid())
                settings.setValue(i.key(), i.value());
            else
                settings.remove(i.key());
        }
        settings.sync();
        return fail(issue("Cannot save the add-on choice. Check your settings folder permissions."));
    }
    m_error.clear();
    emit changed();
    return true;
}
QVariantList AddonRegistry::integrationRecords(const QString &target) const {
    QVariantList result;
    for (const auto &owner : m_rows) {
        if (!owner.value("error").toString().isEmpty())
            continue;
        const auto ownerId = owner.value("id").toString();
        for (const auto &value : owner.value("integrations").toList()) {
            auto row = value.toMap();
            const auto targetId = row.value("target").toString();
            if (!target.isEmpty() && target != targetId)
                continue;
            QString reason;
            const auto provider = find(targetId);
            if (!isEnabled(ownerId))
                reason = issue("Enable integration owner: ") + ownerId;
            else if (!provider || !isEnabled(targetId))
                reason = issue("Enable integration target: ") + targetId;
            else if (version(provider->value("version").toString()).isNull() ||
                     version(provider->value("version").toString()) <
                         version(row.value("minVersion").toString()))
                reason = issue("The integration target version is incompatible.");
            else {
                bool point = false;
                const auto name = row.value("point").toString();
                if (name == "page" || name == "settings") {
                    point = row.value("apiVersion").toInt() == 1 &&
                            (name == "page" ? !provider->value("page").toUrl().isEmpty()
                                            : !provider->value("settings").toList().isEmpty());
                } else {
                    for (const auto &entry : provider->value("extensionPoints").toList()) {
                        const auto declared = entry.toMap();
                        if (declared.value("id") == row.value("point") &&
                            declared.value("apiVersion") == row.value("apiVersion"))
                            point = true;
                    }
                }
                if (!point)
                    reason = issue("The integration point is missing or incompatible.");
            }
            const bool selected =
                PilonSettings()
                    .value(integrationKey(ownerId, row.value("id").toString()), row.value("enabledByDefault", false))
                    .toBool();
            row["ownerId"] = ownerId;
            row["configuredEnabled"] = selected;
            row["available"] = reason.isEmpty();
            row["enabled"] = selected && reason.isEmpty();
            row["reason"] = reason;
            result << row;
        }
    }
    return result;
}
bool AddonRegistry::setIntegrationEnabled(const QString &owner, const QString &id, bool enabled) {
    QVariantMap row;
    for (const auto &entry : integrationRecords()) {
        const auto candidate = entry.toMap();
        if (candidate.value("ownerId") == owner && candidate.value("id") == id) {
            row = candidate;
            break;
        }
    }
    if (row.isEmpty())
        return fail(issue("This integration is unavailable."));
    if (enabled && !row.value("available").toBool())
        return fail(row.value("reason").toString());
    PilonSettings settings;
    const auto key = integrationKey(owner, id);
    const auto previous = settings.value(key);
    settings.setValue(key, enabled);
    settings.sync();
    if (settings.status() != PilonSettings::NoError) {
        if (previous.isValid())
            settings.setValue(key, previous);
        else
            settings.remove(key);
        settings.sync();
        return fail(issue("Cannot save the integration choice."));
    }
    m_error.clear();
    emit changed();
    return true;
}
QVariantList AddonRegistry::lifecycleOwners() const {
    QVariantList result;
    for (auto i = m_receipts.cbegin(); i != m_receipts.cend(); ++i) {
        auto owner = i->toMap();
        const auto row = find(i.key());
        const bool removed = owner.value("removed").toBool();
        owner["lifecycle"] = row ? row->value("lifecycle") : QUrl{};
        owner["missing"] = !removed && !row;
        owner["lifecycleDeclared"] = !removed && owner.value("lifecycleDeclared").toBool();
        auto paths = owner.value("ownedPaths").toMap();
        auto scoped = paths.value("addonData").toList();
        const auto directory = "addon-data/" + i.key();
        if (!scoped.contains(directory))
            scoped << directory;
        paths["addonData"] = scoped;
        owner["ownedPaths"] = paths;
        result << owner;
    }
    return result;
}
bool AddonRegistry::finalizeRemoval(const QString &id, bool cleanupSucceeded, const QVariantMap &paths) {
    if (!m_receipts.contains(id))
        return fail(issue("This add-on has no ownership record."));
    const auto row = find(id);
    const auto receipt = m_receipts.value(id).toMap();
    if (receipt.value("removed").toBool())
        return true;
    const bool selected =
        row ? configured(*row)
            : PilonSettings()
                  .value(enabledKey(id), m_seeded.contains(id) && id != "git" && id != "devices")
                  .toBool();
    if (selected)
        return fail(issue("Disable this add-on before removing it."));
    const auto requiring = dependents(id);
    if (!requiring.isEmpty())
        return fail(issue("Disable these required dependents first: ") + requiring.join(", "));
    if (receipt.value("lifecycleDeclared").toBool() && !cleanupSucceeded)
        return fail(issue("Complete this add-on's account cleanup before removing it."));
    int duplicates = 0;
    for (const auto &entry : m_rows)
        if (entry.value("id") == id)
            ++duplicates;
    if (duplicates > 1)
        return fail(issue("Remove conflicting package files before removing this add-on."));
    QVariantMap retained;
    if (!ownedPaths(paths, &retained))
        return fail(
            issue("Owned paths must be personal, downloads or addonData lists of workspace-relative paths."));
    auto priorPaths = receipt.value("ownedPaths").toMap();
    for (auto i = priorPaths.cbegin(); i != priorPaths.cend(); ++i) {
        auto values = retained.value(i.key()).toList();
        for (const auto &value : i->toList())
            if (!values.contains(value))
                values << value;
        retained[i.key()] = values;
    }
    QVariantMap checked;
    if (!ownedPaths(retained, &checked))
        return fail(issue("The retained ownership paths are too large."));
    const auto filename = receipt.value("file").toString();
    if (!localPath(filename) || filename.contains('/'))
        return fail(issue("This add-on's package path is invalid."));
    const auto path = QDir(m_installed).absoluteFilePath(filename);
    QString directoryError;
    if (!privateDirectory(m_installed, &directoryError))
        return fail(directoryError);
    if (QFileInfo(m_installed).isSymLink() || QFileInfo(path).isSymLink() ||
        (QFileInfo::exists(path) && !QFileInfo(path).isFile()))
        return fail(issue("The installed package is not a regular file."));
    if (QFileInfo::exists(path) &&
        QString::fromLatin1(fileHash(path).toHex()) != receipt.value("hash").toString())
        return fail(issue("The package changed. Restart Pilon before removing it."));
    const auto staged =
        QDir(m_installed).absoluteFilePath(".removed-" + QUuid::createUuid().toString(QUuid::Id128));
    const bool exists = QFileInfo::exists(path);
    if (exists && !QFile::rename(path, staged))
        return fail(issue("Cannot remove the installed package. Check the add-on folder permissions."));
    auto removed = receipt;
    removed["removed"] = true;
    removed["ownedPaths"] = retained;
    m_receipts[id] = removed;
    if (!saveState()) {
        m_receipts[id] = receipt;
        if (exists && !QFile::rename(staged, path))
            return fail(issue("The removal record could not be saved or rolled back. Restore the staged "
                              "package before retrying."));
        return false;
    }
    m_snapshots.remove(path);
    if (exists)
        QFile::remove(staged);
    refresh();
    return true;
}
void AddonRegistry::refresh() {
    m_error.clear();
    const auto watches = qScopeGuard([this] { rearmWatches(); });
    if (!loadState() || !seedPackages())
        return;
    QList<QVariantMap> rows;
    QStringList paths, seen;
    const auto previousSnapshots = m_snapshots;
    QHash<QString, Snapshot> candidates;
    const auto scan = [&](const QString &path, bool builtin, const QString &expectedId) {
        seen << path;
        const auto hash = fileHash(path);
        const auto previous = previousSnapshots.constFind(path);
        if (previous != previousSnapshots.cend() && previous->hash != hash) {
            auto row = previous->row;
            row["restartRequired"] = true;
            rows << row;
            paths << path;
            candidates[path] = previous.value();
            m_error = issue("An installed add-on changed. Restart Pilon to load its new code.");
            return;
        }
        auto row = readPackage(path, builtin, expectedId, hash, true, m_stateDirectory + "/cache");
        row["packagePath"] = path;
        row["builtin"] = builtinIds.contains(row.value("id").toString());
        row["defaultEnabled"] = m_seeded.contains(row.value("id").toString()) && row.value("id") != "git" &&
                                row.value("id") != "devices";
        row["lifecycleDeclared"] = !row.value("lifecycle").toUrl().isEmpty();
        if (row.value("error").toString().isEmpty())
            candidates[path] = {hash, row};
        rows << row;
        paths << path;
    };
    for (const auto &file :
         QDir(m_installed).entryInfoList({"*.piext"}, QDir::Files | QDir::System, QDir::Name))
        scan(file.absoluteFilePath(), false, {});
    for (auto i = previousSnapshots.cbegin(); i != previousSnapshots.cend(); ++i)
        if (!seen.contains(i.key())) {
            auto row = i->row;
            row["restartRequired"] = true;
            rows << row;
            paths << i.key();
            candidates[i.key()] = i.value();
            m_error = issue("An installed add-on changed. Restart Pilon to load its new code.");
        }
    QHash<QString, int> counts;
    for (const auto &row : rows)
        if (row.value("error").toString().isEmpty())
            ++counts[row.value("id").toString()];
    QHash<QString, QString> owners;
    for (auto i = previousSnapshots.cbegin(); i != previousSnapshots.cend(); ++i)
        owners[i->row.value("id").toString()] = i.key();
    QHash<QString, Snapshot> nextSnapshots;
    const auto previousReceipts = m_receipts;
    QStringList present;
    for (int i = 0; i < rows.size(); ++i) {
        auto &row = rows[i];
        const auto id = row.value("id").toString();
        if (counts.value(id) > 1 && owners.value(id) != paths[i])
            row["error"] = issue("Duplicate add-on ID. Remove the conflicting package and refresh.");
        if (row.value("error").toString().isEmpty()) {
            nextSnapshots[paths[i]] = candidates.value(paths[i]);
            present << id;
            auto receipt = m_receipts.value(id).toMap();
            receipt["id"] = id;
            receipt["name"] = row.value("name");
            receipt["file"] = QFileInfo(paths[i]).fileName();
            receipt["lifecycleDeclared"] =
                receipt.value("lifecycleDeclared").toBool() || row.value("lifecycleDeclared").toBool();
            receipt["hash"] = QString::fromLatin1(candidates.value(paths[i]).hash.toHex());
            receipt["removed"] = false;
            m_receipts[id] = receipt;
        }
    }
    if (m_receipts != previousReceipts && !saveState()) {
        m_receipts = previousReceipts;
        return;
    }
    for (auto i = m_receipts.cbegin(); i != m_receipts.cend(); ++i) {
        const auto receipt = i->toMap();
        if (present.contains(i.key()) || receipt.value("removed").toBool())
            continue;
        rows << QVariantMap{
            {"id", i.key()},
            {"name", receipt.value("name")},
            {"description", ""},
            {"version", ""},
            {"page", QUrl{}},
            {"runtime", QUrl{}},
            {"lifecycle", QUrl{}},
            {"settings", QVariantList{}},
            {"dependencies", QVariantList{}},
            {"integrations", QVariantList{}},
            {"extensionPoints", QVariantList{}},
            {"missing", true},
            {"builtin", builtinIds.contains(i.key())},
            {"defaultEnabled", m_seeded.contains(i.key()) && i.key() != "git" && i.key() != "devices"},
            {"lifecycleDeclared", receipt.value("lifecycleDeclared")},
            {"error",
             issue("This add-on package is missing or invalid. Restore its package before cleanup.")}};
    }
    m_snapshots = nextSnapshots;
    m_rows = rows;
    emit changed();
}
bool AddonRegistry::install(const QUrl &url) {
    if (!url.isLocalFile())
        return fail(issue("Choose a local .piext package."));
    QFile source(url.toLocalFile());
    if (!QFileInfo(source).isFile() || QFileInfo(source).isSymLink() ||
        QFileInfo(source).suffix().compare("piext", Qt::CaseInsensitive) != 0)
        return fail(issue("Choose a regular .piext file, not a folder or symbolic link."));
    QString directoryError;
    if (!privateDirectory(m_installed, &directoryError))
        return fail(directoryError);
    QTemporaryFile stage(QDir(m_installed).absoluteFilePath(".install-XXXXXX.piext"));
    if (!source.open(QIODevice::ReadOnly) || !stage.open())
        return fail(issue("Cannot read or stage this package."));
    while (!source.atEnd()) {
        const auto bytes = source.read(1024 * 1024);
        if ((bytes.isEmpty() && source.error() != QFile::NoError) || stage.write(bytes) != bytes.size())
            return fail(issue("Cannot copy this package."));
    }
    if (!stage.flush())
        return fail(issue("Cannot save the staged package."));
    stage.close();
    const auto row = readPackage(stage.fileName(), false, {}, fileHash(stage.fileName()), false);
    if (!row.value("error").toString().isEmpty())
        return fail(row.value("error").toString());
    const auto id = row.value("id").toString();
    const auto destination = QDir(m_installed).absoluteFilePath(id + ".piext");
    for (const auto &existing : m_rows)
        if (existing.value("id") == id && existing.value("error").toString().isEmpty()) {
            bool atDestination = false;
            const auto snapshot = m_snapshots.constFind(destination);
            if (snapshot != m_snapshots.cend() && snapshot->row.value("id") == id)
                atDestination = true;
            if (!atDestination)
                return fail(issue("This add-on ID is already installed under another filename."));
        }
    if (QFileInfo(destination).isSymLink() ||
        (QFileInfo::exists(destination) && !QFileInfo(destination).isFile()))
        return fail(issue("The package destination is not a regular file."));
    QFile staged(stage.fileName());
    QSaveFile output(destination);
    if (!staged.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
        return fail(issue("Cannot write the installed package."));
    output.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    while (!staged.atEnd()) {
        const auto bytes = staged.read(1024 * 1024);
        if ((bytes.isEmpty() && staged.error() != QFile::NoError) || output.write(bytes) != bytes.size())
            return fail(issue("Cannot write the installed package."));
    }
    if (!output.commit())
        return fail(issue("Cannot commit the installed package."));
    refresh();
    return true;
}
QString AddonRegistry::dataDirectory(const QString &id) {
    if (!find(id)) {
        fail(issue("This add-on has no valid registered package."));
        return {};
    }
    QString directoryError;
    if (!privateDirectory(m_data, &directoryError)) {
        fail(directoryError);
        return {};
    }
    const auto path = QDir(m_data).absoluteFilePath(id);
    if (!privateDirectory(path, &directoryError)) {
        fail(directoryError);
        return {};
    }
    return path;
}
bool AddonRegistry::settingsWritable(const QString &id) {
    const auto directory = dataDirectory(id);
    if (directory.isEmpty())
        return false;
    const auto path = directory + "/settings.ini";
    const QFileInfo settings(path);
    if (settings.isSymLink())
        return fail(issue("Add-on settings cannot use symbolic links."));
    if (settings.exists() && (!settings.isFile() || !settings.isWritable()))
        return fail(issue("Cannot write add-on settings. Check the settings file permissions."));
    QSaveFile probe(path);
    if (!probe.open(QIODevice::WriteOnly))
        return fail(
            issue("Cannot stage add-on settings. Check the data folder permissions and available space."));
    probe.cancelWriting();
    if (!m_error.isEmpty()) {
        m_error.clear();
        emit changed();
    }
    return true;
}
QVariant AddonRegistry::setting(const QString &id, const QString &key, const QVariant &fallback) {
    if (!validKey(key)) {
        fail(issue("Use an add-on setting key containing letters, numbers, dots, underscores or hyphens."));
        return fallback;
    }
    const auto directory = dataDirectory(id);
    if (directory.isEmpty())
        return fallback;
    const auto path = directory + "/settings.ini";
    if (QFileInfo(path).isSymLink()) {
        fail(issue("Add-on settings cannot use symbolic links."));
        return fallback;
    }
    PilonSettings settings(path, PilonSettings::IniFormat);
    const auto value = settings.value(key, fallback);
    if (settings.status() != PilonSettings::NoError) {
        fail(issue("Cannot read add-on settings."));
        return fallback;
    }
    return value;
}
bool AddonRegistry::setSetting(const QString &id, const QString &key, const QVariant &value) {
    const auto stored =
        value.metaType() == QMetaType::fromType<QJSValue>() ? value.value<QJSValue>().toVariant() : value;
    if (!validKey(key) || !preferenceValue(stored))
        return fail(issue("Use a valid setting key and a JSON-compatible value."));
    const auto directory = dataDirectory(id);
    if (directory.isEmpty())
        return false;
    const auto path = directory + "/settings.ini";
    if (QFileInfo(path).isSymLink())
        return fail(issue("Add-on settings cannot use symbolic links."));
    const bool existed = QFileInfo::exists(path);
    PilonSettings settings(path, PilonSettings::IniFormat);
    const auto old = settings.value(key);
    const bool hadKey = settings.contains(key);
    settings.setValue(key, stored);
    settings.sync();
    if (settings.status() != PilonSettings::NoError ||
        !QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner)) {
        if (hadKey)
            settings.setValue(key, old);
        else
            settings.remove(key);
        settings.sync();
        if (!existed)
            QFile::remove(path);
        return fail(issue("Cannot save add-on settings. Check the data folder permissions."));
    }
    m_error.clear();
    emit changed();
    return true;
}
