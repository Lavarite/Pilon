#include "piconfdocument.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QStringDecoder>
#include <functional>

const QString PiconfDocument::MainPath = QStringLiteral("config/pilon.piconf");

namespace {
using Key = QStringList;
struct Entry {
    QString file;
    int line = 0; // One-based, including comments and blank lines.
    qsizetype begin = 0, end = 0;
    QVariant value;
};
struct File {
    QStringList lines;
    QMap<Key, Entry> entries;
    QMap<Key, int> sections; // Most recent header line. Sections may be reopened.
    bool bom = false;
};
struct Scope {
    QMap<Key, Entry> entries;
    QMap<Key, QVariant> resolved;
    QMap<Key, qsizetype> weights;
    QSet<QString> includes;
    QVariantMap tree;
};

QString where(const QString &file, int line, const QString &description) {
    return QStringLiteral("%1:%2: %3").arg(file).arg(qMax(1, line)).arg(description);
}
bool bare(QChar c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'-' ||
           c == u'_';
}
qsizetype stringEnd(QStringView text, qsizetype start) {
    for (qsizetype i = start + 1; i < text.size(); ++i) {
        if (text[i] == u'\\')
            ++i;
        else if (text[i] == u'"')
            return i + 1;
    }
    return -1;
}
bool readKey(QStringView input, Key &key) {
    input = input.trimmed();
    key.clear();
    while (!input.isEmpty()) {
        QString part;
        qsizetype count = 0;
        if (input.front() == u'"') {
            count = stringEnd(input, 0);
            if (count < 0)
                return false;
            const auto decoded = QJsonValue::fromJson(input.first(count).toUtf8());
            if (!decoded.isString())
                return false;
            part = decoded.toString();
        } else {
            while (count < input.size() && bare(input[count]))
                ++count;
            part = input.first(count).toString();
        }
        if (part.isEmpty() || part.size() > 256 || part.contains(QChar::Null) || key.size() >= 32)
            return false;
        key.push_back(part);
        input = input.sliced(count).trimmed();
        if (input.isEmpty())
            return true;
        if (input.front() != u'.')
            return false;
        input = input.sliced(1).trimmed();
    }
    return false;
}
QString keyText(const Key &key) {
    QStringList parts;
    for (const auto &part : key)
        parts << (!part.isEmpty() && std::all_of(part.cbegin(), part.cend(), bare)
                      ? part
                      : QString::fromUtf8(QJsonValue(part).toJson(QJsonValue::JsonFormat::Compact)));
    return parts.join(u'.');
}
bool merge(QVariantMap &map, const Key &key, const QVariant &value, int index = 0) {
    const auto &part = key[index];
    if (index + 1 == key.size()) {
        if (map.contains(part))
            return false;
        map.insert(part, value);
        return true;
    }
    if (map.contains(part) && map.value(part).typeId() != QMetaType::QVariantMap)
        return false;
    auto child = map.value(part).toMap();
    if (!merge(child, key, value, index + 1))
        return false;
    map.insert(part, child);
    return true;
}
bool uniqueJsonKeys(QStringView value) {
    QList<QSet<QString>> objects;
    for (qsizetype i = 0; i < value.size(); ++i) {
        if (value[i] == u'{')
            objects.append(QSet<QString>());
        else if (value[i] == u'}')
            objects.removeLast();
        else if (value[i] == u'"') {
            const qsizetype end = stringEnd(value, i); // JSON syntax was validated by Qt.
            qsizetype next = end;
            while (next < value.size() && value[next].isSpace())
                ++next;
            if (next < value.size() && value[next] == u':') {
                const auto key = QJsonValue::fromJson(value.sliced(i, end - i).toUtf8()).toString();
                if (objects.last().contains(key))
                    return false;
                objects.last().insert(key);
            }
            i = end - 1;
        }
    }
    return true;
}
bool parseFile(const QString &path, QByteArrayView bytes, File &file, QString &error) {
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString text = decoder(bytes);
    auto fail = [&](int line, const QString &why) {
        error = where(path, line, why);
        return false;
    };
    if (decoder.hasError() || text.contains(QChar::Null))
        return fail(1, "Expected UTF-8 configuration text.");
    file.bom = bytes.startsWith("\xef\xbb\xbf");
    file.lines = text.split(u'\n');
    Key section;
    for (int n = 0; n < file.lines.size(); ++n) {
        const QStringView line(file.lines[n]);
        qsizetype end = line.size(), equal = -1;
        for (qsizetype i = 0; i < end; ++i) {
            if (line[i] == u'"') {
                const auto after = stringEnd(line, i);
                if (after < 0)
                    return fail(n + 1, "Unterminated quoted string.");
                i = after - 1;
            } else if (line[i] == u'#') {
                end = i;
                break;
            } else if (line[i] == u'=' && equal < 0)
                equal = i;
        }
        const auto clean = line.first(end).trimmed();
        if (clean.isEmpty())
            continue;
        if (clean.front() == u'[') {
            if (!clean.endsWith(u']') || !readKey(clean.sliced(1, clean.size() - 2), section))
                return fail(n + 1, "Invalid section name.");
            file.sections[section] = n + 1;
            continue;
        }
        Key parts;
        if (equal < 0 || !readKey(line.first(equal), parts))
            return fail(n + 1, "Expected a key and an equals sign.");
        const Key key = section + parts;
        if (file.entries.contains(key))
            return fail(n + 1, "Duplicate key " + keyText(key));
        qsizetype begin = equal + 1;
        while (begin < end && line[begin].isSpace())
            ++begin;
        while (end > begin && line[end - 1].isSpace())
            --end;
        const auto value = line.sliced(begin, end - begin);
        QJsonParseError jsonError;
        const auto json = QJsonValue::fromJson(value.toUtf8(), &jsonError);
        if (jsonError.error != QJsonParseError::NoError)
            return fail(n + 1, "Invalid value: " + jsonError.errorString());
        if ((json.isArray() || json.isObject()) && !uniqueJsonKeys(value))
            return fail(n + 1, "Duplicate object key.");
        file.entries.insert(key, {path, n + 1, begin, end, json.toVariant()});
        if (file.entries.size() > 16384)
            return fail(n + 1, "Too many configuration keys.");
    }
    return true;
}
bool portablePart(const QString &part) {
    if (part.isEmpty() || part == "." || part == ".." || part.size() > 255)
        return false;
    return std::none_of(part.cbegin(), part.cend(),
                        [](QChar c) { return c.unicode() < 32 || c == u':' || c == u'\\' || c == u'\x7f'; });
}
QString relativeTarget(const QString &from, const QString &link) {
    if (link.isEmpty() || link.contains(u'\\') || link.contains(u':') || QDir::isAbsolutePath(link))
        return {};
    const auto resolved = QDir::cleanPath(from.section(u'/', 0, -2) + u'/' + link);
    return PiconfDocument::validPath(resolved) ? resolved : QString();
}
QVariant escapedValue(const QVariant &value) {
    if (value.typeId() == QMetaType::QString) {
        auto text = value.toString();
        return text.replace(u'$', QStringLiteral("$$"));
    }
    if (value.typeId() == QMetaType::QVariantMap) {
        auto map = value.toMap();
        for (auto it = map.begin(); it != map.end(); ++it)
            it.value() = escapedValue(it.value());
        return map;
    }
    if (value.typeId() == QMetaType::QStringList || value.typeId() == QMetaType::QVariantList) {
        auto list = value.toList();
        for (auto &item : list)
            item = escapedValue(item);
        return list;
    }
    return value;
}
QString literal(const QVariant &value) {
    return QString::fromUtf8(
        QJsonValue::fromVariant(escapedValue(value)).toJson(QJsonValue::JsonFormat::Compact));
}
Key soleReference(const QVariant &value) {
    if (value.typeId() != QMetaType::QString)
        return {};
    const auto text = value.toString();
    Key key;
    if (!text.startsWith("${") || text.indexOf(u'}') != text.size() - 1 ||
        !readKey(QStringView(text).sliced(2, text.size() - 3), key) || key.first() != "vars" ||
        key.size() < 2)
        return {};
    return key;
}
} // namespace

struct PiconfDocument::Data {
    Files bytes;
    QMap<QString, File> documents;
    QMap<QString, Scope> scopes;
    QMap<QString, QString> addonFiles;
    QStringList missingConfigs;
    qsizetype totalBytes = 0;
    qsizetype expandedBytes = 0;
    using Reader = std::function<bool(const QString &, QByteArray &, QString &)>;
    bool collect(const QString &path, Scope &scope, const Reader &read, int depth, QString &error) {
        if (depth > 32 || scope.includes.contains(path)) {
            error = where(path, 1, "Repeated or cyclic include, or too many nested includes.");
            return false;
        }
        scope.includes.insert(path);
        if (!documents.contains(path)) {
            if (documents.size() >= MaxFiles) {
                error = where(path, 1, "Too many configuration files.");
                return false;
            }
            QByteArray content;
            if (!read(path, content, error))
                return false;
            totalBytes += content.size();
            if (content.size() > MaxFileBytes || totalBytes > MaxBytes) {
                error = where(path, 1, "Configuration size limit exceeded.");
                return false;
            }
            File parsed;
            if (!parseFile(path, content, parsed, error))
                return false;
            documents.insert(path, parsed);
            bytes.insert(path, content);
        }
        const auto &entries = documents.constFind(path)->entries;
        const auto version = entries.constFind({"format"});
        if (version != entries.end() &&
            (version->value.typeId() != QMetaType::LongLong || version->value.toLongLong() != 1)) {
            error = where(path, version->line, "Expected format = 1.");
            return false;
        }
        if (path == MainPath && version == entries.end()) {
            error = where(path, 1, "Missing format = 1.");
            return false;
        }
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it.key() == Key{"include"} || (it.key() == Key{"format"} && depth > 0))
                continue;
            if (scope.entries.contains(it.key())) {
                error = where(path, it->line, "Duplicate key " + keyText(it.key()));
                return false;
            }
            scope.entries.insert(it.key(), *it);
        }
        const auto include = entries.constFind({"include"});
        if (include == entries.end())
            return true;
        if (include->value.typeId() != QMetaType::QVariantList) {
            error = where(path, include->line, "Include must be a list of relative filenames.");
            return false;
        }
        for (const auto &value : include->value.toList()) {
            const auto target =
                value.typeId() == QMetaType::QString ? relativeTarget(path, value.toString()) : QString();
            if (target.isEmpty()) {
                error = where(path, include->line, "Invalid include path.");
                return false;
            }
            if (!collect(target, scope, read, depth + 1, error)) {
                error = where(path, include->line, error);
                return false;
            }
        }
        if (depth == 0)
            scope.entries.insert({"include"}, *include);
        return true;
    }
    bool build(const Reader &read, QString &error, const QString &workspace = {});
    bool resolve(const QString &scopeId, QString &error);
};

bool PiconfDocument::validPath(const QString &path) {
    if (path.size() > 1024 || !path.endsWith(".piconf"))
        return false;
    const auto parts = path.split(u'/');
    if (!(parts.size() >= 2 && parts.first() == "config") &&
        !(parts.size() >= 3 && parts.first() == "addon-data"))
        return false;
    return std::all_of(parts.cbegin(), parts.cend(), portablePart);
}

QString PiconfDocument::storagePath(const QString &relative) {
    // The portable configuration format keeps its original paths.
    const auto parts = relative.split('/');
    if (parts.size() >= 2 && parts.first() == "addon-data")
        return "addons/" + parts[1] + "/data" + relative.mid(QStringView(u"addon-data/").size() + parts[1].size());
    return relative;
}

bool PiconfDocument::Data::resolve(const QString &scopeId, QString &error) {
    auto &scope = scopes[scopeId];
    const auto mainIt = scopes.constFind(MainPath);
    const auto &main = mainIt.value();
    QSet<Key> visiting;

    std::function<bool(const Key &, QVariant &, qsizetype &)> resolveKey;
    std::function<bool(const Entry &, const QVariant &, QVariant &, qsizetype &, int)> walk;
    walk = [&](const Entry &e, const QVariant &raw, QVariant &out, qsizetype &size, int depth) {
        auto fail = [&](const QString &msg) {
            error = where(e.file, e.line, msg);
            return false;
        };
        if (depth > 64)
            return fail("Value nested too deeply.");
        if (raw.typeId() == QMetaType::QString) {
            const QString input = raw.toString();
            QString result;
            for (qsizetype i = 0; i < input.size();) {
                const auto dollar = input.indexOf(u'$', i);
                if (dollar < 0) {
                    result += QStringView(input).sliced(i);
                    break;
                }
                result += QStringView(input).sliced(i, dollar - i);
                i = dollar + 1;
                if (i < input.size() && input[i] == u'$') {
                    result += u'$';
                    ++i;
                    continue;
                }
                if (i == input.size() || input[i] != u'{') {
                    result += u'$';
                    continue;
                }
                const auto close = input.indexOf(u'}', i + 1);
                Key key;
                if (close < 0 || !readKey(QStringView(input).sliced(i + 1, close - i - 1), key) ||
                    key.first() != "vars" || key.size() < 2)
                    return fail("Invalid variable reference.");
                QVariant replacement;
                qsizetype weight = 0;
                if (!scope.entries.contains(key) && !(scopeId != MainPath && main.resolved.contains(key)))
                    return fail("Unknown variable " + keyText(key));
                if (!resolveKey(key, replacement, weight))
                    return false;
                i = close + 1;
                if (dollar == 0 && i == input.size()) {
                    out = replacement;
                    size += weight;
                    return true;
                }
                if (replacement.typeId() == QMetaType::QVariantList ||
                    replacement.typeId() == QMetaType::QVariantMap || replacement.isNull())
                    return fail("Interpolation requires a scalar variable.");
                result += replacement.toString();
                if (result.size() > MaxFileBytes)
                    return fail("Expanded string is too large.");
            }
            out = result;
            size += result.size() + 1;
        } else if (raw.typeId() == QMetaType::QVariantList) {
            QVariantList list;
            for (const auto &item : raw.toList()) {
                QVariant child;
                if (!walk(e, item, child, size, depth + 1))
                    return false;
                list.append(child);
            }
            out = list;
            ++size;
        } else if (raw.typeId() == QMetaType::QVariantMap) {
            auto map = raw.toMap();
            for (auto it = map.begin(); it != map.end(); ++it) {
                QVariant child;
                size += it.key().size() + 1;
                if (!walk(e, it.value(), child, size, depth + 1))
                    return false;
                it.value() = child;
            }
            out = map;
            ++size;
        } else {
            out = raw;
            ++size;
        }
        if (size > MaxBytes)
            return fail("Expanded configuration is too large.");
        return true;
    };
    resolveKey = [&](const Key &key, QVariant &value, qsizetype &size) {
        if (scope.resolved.contains(key)) {
            value = scope.resolved.value(key);
            size = scope.weights.value(key);
            return true;
        }
        if (!scope.entries.contains(key)) {
            value = main.resolved.value(key);
            size = main.weights.value(key);
            return true;
        }
        const auto entryIt = scope.entries.constFind(key);
        const auto &entry = entryIt.value();
        if (visiting.contains(key) || visiting.size() >= 64) {
            error =
                where(entry.file, entry.line, "Cyclic or overly nested variable reference: " + keyText(key));
            return false;
        }
        visiting.insert(key);
        if (!walk(entry, entry.value, value, size, 0))
            return false;
        visiting.remove(key);
        expandedBytes += size;
        if (expandedBytes > MaxBytes) {
            error = where(entry.file, entry.line, "Expanded configuration is too large.");
            return false;
        }
        scope.resolved.insert(key, value);
        scope.weights.insert(key, size);
        return true;
    };
    for (auto it = scope.entries.cbegin(); it != scope.entries.cend(); ++it) {
        Key prefix = it.key();
        while (prefix.size() > 1) {
            prefix.removeLast();
            if (scope.entries.contains(prefix)) {
                error = where(it->file, it->line, "Key conflicts with a section: " + keyText(prefix));
                return false;
            }
        }
        QVariant value;
        qsizetype size = 0;
        if (!resolveKey(it.key(), value, size))
            return false;
        if (!merge(scope.tree, it.key(), value)) {
            error = where(it->file, it->line, "Key conflicts with a section: " + keyText(it.key()));
            return false;
        }
    }
    return true;
}

bool PiconfDocument::Data::build(const Reader &read, QString &error, const QString &workspace) {
    if (!collect(MainPath, scopes[MainPath], read, 0, error) || !resolve(MainPath, error))
        return false;
    const auto hostIt = scopes.constFind(MainPath);
    const auto &host = hostIt.value();
    const QVariant addons = host.tree.value("addons");
    if (addons.isValid() && addons.typeId() != QMetaType::QVariantMap) {
        error = where(MainPath, 1, "Expected add-on declarations.");
        return false;
    }
    const auto declarations = addons.toMap();
    for (auto it = declarations.begin(); it != declarations.end(); ++it) {
        const auto id = it.key();
        const auto declaration = it.value().toMap();
        if (!declaration.contains("config"))
            continue;
        Key key = {"addons", id, "config"};
        Key ancestor = key;
        while (!ancestor.isEmpty() && !host.entries.contains(ancestor))
            ancestor.removeLast();
        if (ancestor.isEmpty()) {
            error = where(MainPath, 1, "Missing add-on configuration declaration.");
            return false;
        }
        const auto entryIt = host.entries.constFind(ancestor);
        const auto &entry = entryIt.value();
        QVariant raw = entry.value;
        for (auto i = ancestor.size(); i < key.size(); ++i)
            raw = raw.toMap().value(key[i]);
        const auto target =
            raw.typeId() == QMetaType::QString ? relativeTarget(entry.file, raw.toString()) : QString();
        if (!portablePart(id) || !target.startsWith("addon-data/" + id + "/")) {
            error = where(entry.file, entry.line,
                          "Expected a configuration file in this add-on’s data directory.");
            return false;
        }
        if (!workspace.isEmpty()) {
            const QFileInfo folder(workspace + "/addons/" + id);
            if (!folder.exists() && !folder.isSymLink() && !QFileInfo(workspace + "/addons").isSymLink()) {
                missingConfigs << id;
                continue;
            }
        }
        addonFiles.insert(id, target);
        if (!collect(target, scopes[target], read, 0, error) || !resolve(target, error)) {
            error = where(entry.file, entry.line, error);
            return false;
        }
        for (const auto &include : scopes.constFind(target)->includes) {
            if (include.startsWith("addon-data/") && !include.startsWith("addon-data/" + id + "/")) {
                error = where(entry.file, entry.line,
                              "Add-on include refers to another add-on’s private directory.");
                return false;
            }
        }
    }
    return true;
}

bool PiconfDocument::parse(const Files &files, QString &error) {
    error.clear();
    auto result = std::make_shared<Data>();
    if (!result->build(
            [&](const QString &path, QByteArray &bytes, QString &issue) {
                const auto it = files.constFind(path);
                if (it == files.end()) {
                    issue = where(path, 1, "Missing configuration file.");
                    return false;
                }
                bytes = *it;
                return true;
            },
            error))
        return false;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        if (!result->bytes.contains(it.key())) {
            error = where(it.key(), 1, "File is not referenced by the main configuration.");
            return false;
        }
    m_data = std::move(result);
    return true;
}
bool PiconfDocument::load(const QString &workspace, QString &error) {
    error.clear();
    const QString root = QFileInfo(workspace).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        error = "Workspace directory is unavailable.";
        return false;
    }
    auto result = std::make_shared<Data>();
    if (!result->build(
            [&](const QString &path, QByteArray &bytes, QString &issue) {
                QString physical = root;
                for (const auto &part : storagePath(path).split(u'/')) {
                    physical = QDir(physical).filePath(part);
                    if (QFileInfo(physical).isSymLink()) {
                        issue = where(path, 1, "Symbolic links cannot be included.");
                        return false;
                    }
                }
                QFile file(physical);
                if (!QFileInfo(physical).isFile() || !file.open(QIODevice::ReadOnly)) {
                    issue = where(path, 1, "Cannot read configuration file.");
                    return false;
                }
                bytes = file.read(MaxFileBytes + 1);
                if (file.error() != QFileDevice::NoError) {
                    issue = where(path, 1, file.errorString());
                    return false;
                }
                return true;
            },
            error, root))
        return false;
    m_data = std::move(result);
    return true;
}
QStringList PiconfDocument::missingAddonConfigs() const {
    return m_data ? m_data->missingConfigs : QStringList{};
}
const PiconfDocument::Files &PiconfDocument::files() const {
    static const Files empty;
    return m_data ? m_data->bytes : empty;
}
QString PiconfDocument::addonFile(const QString &id) const {
    return m_data ? m_data->addonFiles.value(id) : QString();
}
QVariantMap PiconfDocument::values(const QString &path) const {
    if (!m_data) return {};
    const auto scope = m_data->scopes.constFind(path);
    return scope == m_data->scopes.cend() ? QVariantMap{} : scope->tree;
}

QByteArray PiconfDocument::encode(const QVariantMap &values) {
    QStringList lines;
    std::function<void(const Key &, const QVariantMap &)> section;
    section = [&](const Key &key, const QVariantMap &map) {
        bool header = false;
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (it->typeId() == QMetaType::QVariantMap && !it->toMap().isEmpty())
                continue;
            if (!key.isEmpty() && !header) {
                if (!lines.isEmpty())
                    lines << QString();
                lines << "[" + keyText(key) + "]";
                header = true;
            }
            lines << keyText({it.key()}) + " = " + literal(*it);
        }
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (it->typeId() == QMetaType::QVariantMap && !it->toMap().isEmpty())
                section(key + Key{it.key()}, it->toMap());
        }
    };
    section({}, values);
    return (lines.join(u'\n') + u'\n').toUtf8();
}
bool PiconfDocument::setValue(const QString &path, const QStringList &keys, const QVariant &value,
                              QString &error) {
    return apply({{path, keys, value}}, {}, error);
}
namespace {
QVariant changeChild(const QVariant &root, const Key &key, const QVariant &value, int at = 0) {
    if (at == key.size())
        return value;
    auto map = root.toMap();
    auto changed = changeChild(map.value(key[at]), key, value, at + 1);
    if (changed.isValid())
        map.insert(key[at], changed);
    else
        map.remove(key[at]);
    return map;
}
struct Changes {
    QMap<int, QVariant> replacements;
    QMap<Key, QVariant> additions;
};
QByteArray rewrite(const File &file, const Changes &changes) {
    const QString newline = std::any_of(file.lines.begin(), file.lines.end(),
                                        [](const auto &line) { return line.endsWith(u'\r'); })
                                ? QStringLiteral("\r\n")
                                : QStringLiteral("\n");
    // Existing lines retain their exact whitespace and line endings.
    QStringList lines = file.lines;
    QSet<int> removed;
    for (const auto &entry : file.entries) {
        const auto replacement = changes.replacements.constFind(entry.line);
        if (replacement == changes.replacements.end())
            continue;
        auto &line = lines[entry.line - 1];
        if (replacement->isValid())
            line.replace(entry.begin, entry.end - entry.begin,
                         QString::fromUtf8(
                             QJsonValue::fromVariant(*replacement).toJson(QJsonValue::JsonFormat::Compact)));
        else if (line.sliced(entry.end).trimmed().startsWith(u'#'))
            line = line.sliced(entry.end);
        else
            removed.insert(entry.line - 1);
    }
    QMap<int, QStringList> insertions;
    QMap<Key, QStringList> newSections;
    for (auto it = changes.additions.begin(); it != changes.additions.end(); ++it) {
        if (!it->isValid())
            continue;
        Key key = it.key();
        const auto leaf = key.takeLast();
        const QString text = keyText({leaf}) + " = " + literal(*it);
        if (key.isEmpty() || file.sections.contains(key)) {
            const int start = file.sections.value(key);
            int end = lines.size();
            for (const auto next : file.sections)
                if (next > start)
                    end = qMin(end, next - 1);
            // Reopened sections may have an earlier header not present in sections.
            for (int i = start; i < end; ++i)
                if (QStringView(lines[i]).trimmed().startsWith(u'[')) {
                    end = i;
                    break;
                }
            while (end > start && lines[end - 1].trimmed().isEmpty())
                --end;
            insertions[end] << text;
        } else
            newSections[key] << text;
    }
    QString result;
    auto appendLine = [&](const QString &line) {
        if (!result.isEmpty() && !result.endsWith(u'\n'))
            result += newline;
        result += line + newline;
    };
    for (int i = 0; i <= lines.size(); ++i) {
        for (const auto &text : insertions.value(i))
            appendLine(text);
        if (i == lines.size() || removed.contains(i))
            continue;
        result += lines[i];
        if (i + 1 < lines.size())
            result += u'\n';
    }
    for (auto it = newSections.begin(); it != newSections.end(); ++it) {
        if (!result.isEmpty() && !result.endsWith(newline + newline))
            appendLine({});
        appendLine("[" + keyText(it.key()) + "]");
        for (const auto &text : it.value())
            appendLine(text);
    }
    return (file.bom ? QByteArray("\xef\xbb\xbf") : QByteArray()) + result.toUtf8();
}
} // namespace
bool PiconfDocument::apply(const QList<Edit> &edits, const Files &newFiles, QString &error) {
    error.clear();
    if (!m_data) {
        error = "Load a configuration before editing it.";
        return false;
    }
    Files updated = m_data->bytes;
    QMap<QString, Changes> changes;
    for (auto it = newFiles.begin(); it != newFiles.end(); ++it) {
        if (!validPath(it.key()) || updated.contains(it.key())) {
            error = where(it.key(), 1, "Invalid or existing new file.");
            return false;
        }
        updated.insert(it.key(), it.value());
    }
    for (const auto &edit : edits) {
        if (!m_data->scopes.contains(edit.path) || edit.keys.isEmpty() || edit.keys.size() > 32) {
            error = where(edit.path, 1, "Unknown configuration owner or empty key.");
            return false;
        }
        for (const auto &part : edit.keys)
            if (part.isEmpty() || part.size() > 256 || part.contains(QChar::Null)) {
                error = where(edit.path, 1, "Invalid configuration key.");
                return false;
            }
        const auto scopeIt = m_data->scopes.constFind(edit.path);
        const auto &scope = scopeIt.value();
        Key key = edit.keys;
        // A map written as an inline JSON object owns its member assignments as well.
        while (!key.isEmpty() && !scope.entries.contains(key))
            key.removeLast();
        if (key.isEmpty()) {
            if (edit.value.isValid()) {
                bool hadChildren = false;
                if (edit.value.typeId() == QMetaType::QVariantMap) {
                    const auto following = scope.entries.lowerBound(edit.keys);
                    hadChildren = following != scope.entries.end() &&
                                  following.key().mid(0, edit.keys.size()) == edit.keys;
                }
                if (hadChildren) {
                    error = where(edit.path, 1, "Edit the individual keys in this section.");
                    return false;
                }
                changes[edit.path].additions.insert(edit.keys, edit.value);
            } else {
                for (auto it = scope.entries.lowerBound(edit.keys);
                     it != scope.entries.end() && it.key().mid(0, edit.keys.size()) == edit.keys; ++it)
                    changes[it->file].replacements.insert(it->line, {});
            }
            continue;
        }
        const Scope *origin = &scope;
        auto entry = origin->entries.value(key);
        const auto suffix = edit.keys.mid(key.size());
        QVariant existing = origin->resolved.value(key);
        for (const auto &part : suffix)
            existing = existing.toMap().value(part);
        if (existing.isValid() == edit.value.isValid() &&
            QJsonValue::fromVariant(existing) == QJsonValue::fromVariant(edit.value))
            continue;
        // A simple reference is a shared setting. Removing the key removes the binding only.
        while (edit.value.isValid() || !suffix.isEmpty()) {
            const Key variable = soleReference(entry.value);
            if (variable.isEmpty())
                break;
            if (!origin->entries.contains(variable))
                origin = &m_data->scopes.constFind(MainPath).value();
            entry = origin->entries.value(variable);
        }
        QVariant rawChange = escapedValue(edit.value);
        if (!suffix.isEmpty()) {
            const QVariant raw = changes[entry.file].replacements.value(entry.line, entry.value);
            if (raw.typeId() != QMetaType::QVariantMap) {
                error = where(entry.file, entry.line, "Cannot add a key to a scalar.");
                return false;
            }
            rawChange = changeChild(raw, suffix, rawChange);
        }
        changes[entry.file].replacements.insert(entry.line, rawChange);
    }
    if (changes.isEmpty() && newFiles.isEmpty())
        return true;
    for (auto it = changes.begin(); it != changes.end(); ++it)
        updated[it.key()] = rewrite(m_data->documents.constFind(it.key()).value(), it.value());
    PiconfDocument staged;
    if (!staged.parse(updated, error)) return false;
    for (const auto &edit : edits) {
        QVariant resolved = staged.values(edit.path);
        for (const auto &key : edit.keys) resolved = resolved.toMap().value(key);
        if (resolved.isValid() != edit.value.isValid() ||
            QJsonValue::fromVariant(resolved) != QJsonValue::fromVariant(edit.value)) {
            error = where(edit.path, 1, "Conflicting changes for " + keyText(edit.keys) + '.');
            return false;
        }
    }
    m_data = std::move(staged.m_data);
    return true;
}
