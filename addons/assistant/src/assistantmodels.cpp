#include "../../../src/core/workspace/pilonworkspace.h"
#include "assistantmodels.h"
#include "assistanthubcredentials.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSet>
#include <QStorageInfo>
#include <QStandardPaths>
#include <QUrl>
#include <QtEndian>
#include <QtConcurrentRun>
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace {
constexpr qint64 GiB = 1024LL * 1024 * 1024;
constexpr qsizetype audioHeaderLimit = 256 * 1024;

bool safePath(const QString &path)
{
    return !path.isEmpty() && !QDir::isAbsolutePath(path) && !path.contains('\\')
        && !path.split('/').contains("..") && !path.split('/').contains(".")
        && !path.contains(QChar::Null);
}

bool safeAudioPath(const QString &root, const QString &path)
{
    if (!safePath(path)) return false;
    QDir directory(root);
    for (const auto &part : path.split('/')) {
        if (part.isEmpty()) return false;
        const QFileInfo entry(directory.filePath(part));
        if (entry.isSymLink()) return false;
        directory.setPath(entry.filePath());
    }
    return true;
}

QStringList languageIds(const QJsonValue &value)
{
    QStringList languages;
    static const QRegularExpression code("^[a-zA-Z]{2,3}(?:-[a-zA-Z0-9]{2,8}){0,3}$");
    const auto entries = value.isString() ? QJsonArray{value} : value.toArray();
    for (const auto &entry : entries) {
        if (languages.size() == 128) break;
        const auto id = entry.toString().toLower();
        if (id == "multilingual" || code.match(id).hasMatch()) languages.append(id);
    }
    languages.removeDuplicates();
    return languages;
}

QStringList languageNames(const QStringList &ids, bool supertonic = false)
{
    QStringList names;
    for (const auto &id : ids) {
        const auto language = QLocale(id).language();
        if (supertonic && id == "na") names.append(QObject::tr("Other"));
        else if (id == "multilingual") names.append(QObject::tr("Multilingual"));
        else names.append(language == QLocale::C ? id : QLocale::languageToString(language));
    }
    names.removeDuplicates();
    return names;
}

const QStringList supertonicLanguages{"en", "ar", "bg", "cs", "da", "de", "el", "es", "et", "fi", "fr", "hi",
    "hr", "hu", "id", "it", "ja", "ko", "lt", "lv", "nl", "pl", "pt", "ro", "ru", "sk", "sl", "sv", "tr", "uk", "vi"};
const QStringList supertonicPresets{"M1", "M2", "M3", "M4", "M5", "F1", "F2", "F3", "F4", "F5"};

QVariantMap speechSettings(const QString &engine, const QVariantMap &model = {})
{
    if (engine == "breeze_tts") return {{"settings", QStringList{"description", "speed"}}};
    if (engine == "gradium_phonon") return {{"settings", QStringList{"voiceEmbedding", "temperature"}}, {"streaming", true}};
    if (engine != "supertonic") return {};
    QVariantList languages;
    const auto languageOptions = model.value("languageIds", supertonicLanguages + QStringList{"na"}).toStringList();
    for (const auto &id : languageOptions)
        languages.append(QVariantMap{{"id", id}, {"name", languageNames({id}, true).first()}});
    return {{"settings", QStringList{"preset", "language", "speed"}}, {"languages", languages},
        {"presets", model.value("presetIds", supertonicPresets).toStringList()}};
}

QStringList embeddedSupertonicPresets(const QJsonObject &spec)
{
    for (const auto &value : spec.value("sources").toArray()) {
        const auto source = value.toObject();
        if (source.value("format").toString() != "gguf") continue;
        const QString prefix = "voice_style_";
        static const QRegularExpression valid("^[A-Za-z0-9][A-Za-z0-9_-]{0,31}$");
        QStringList found, ordered;
        for (const auto &key : source.value("files").toObject().keys()) {
            if (!key.startsWith(prefix)) continue;
            const auto id = key.sliced(prefix.size());
            if (valid.match(id).hasMatch()) found.append(id);
        }
        for (const auto &id : supertonicPresets)
            if (found.removeOne(id)) ordered.append(id);
        return ordered + found;
    }
    return {};
}

QString outputEngine(const QString &name)
{
    static const QRegularExpression supertonic("(?:^|[^a-z0-9])supertonic(?:[^a-z]|$)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression breeze("(?:^|[^a-z0-9])breeze[-_ ]?tts(?:[^a-z]|$)", QRegularExpression::CaseInsensitiveOption);
    if (supertonic.match(name).hasMatch()) return "supertonic";
    if (breeze.match(name).hasMatch()) return "breeze_tts";
    return {};
}

QVariantMap audioFile(const QJsonObject &item)
{
    const auto path = item.value("rfilename").toString();
    const auto lfs = item.value("lfs").toObject();
    const auto size = item.value("size").toInteger(lfs.value("size").toInteger());
    const auto digest = lfs.value("sha256").toString().toLower();
    static const QRegularExpression sha256("^[a-f0-9]{64}$");
    if (!safePath(path) || path.split('/').contains(QString{}) || size <= 0
        || (!digest.isEmpty() && !sha256.match(digest).hasMatch())) return {};
    return {{"path", path}, {"size", size}, {"sha256", digest}};
}

QVariantMap phononVariant(const QJsonArray &siblings)
{
    // The pinned audio.cpp loader needs the original checkpoint and its three sidecar files.
    const QStringList required{"model.safetensors", "config.json", "tokenizer.model", "default-voice.safetensors"};
    QVariantList files;
    qint64 bytes = 0;
    for (const auto &name : required) {
        QVariantMap file;
        for (const auto &item : siblings) {
            if (item.toObject().value("rfilename").toString() != name) continue;
            file = audioFile(item.toObject());
            break;
        }
        const auto size = file.value("size").toLongLong();
        if (size <= 0 || size > std::numeric_limits<qint64>::max() - bytes
            || (name == "config.json" && size > audioHeaderLimit)) return {};
        file["format"] = "phonon-checkpoint";
        files.append(file);
        bytes += size;
    }
    return {{"name", QObject::tr("Safetensors checkpoint, %1").arg(QLocale().formattedDataSize(bytes))},
        {"filename", required.first()}, {"bytes", bytes}, {"format", "phonon-checkpoint"},
        {"engine", "gradium_phonon"}, {"files", files}};
}

bool gatedRepository(const QJsonObject &metadata)
{
    const auto access = metadata.value("gated");
    return access.toBool() || access.toString() == "auto" || access.toString() == "manual";
}

QVariantMap describeAudioRepository(const QJsonObject &metadata, const QString &kind)
{
    const auto id = metadata.value("id").toString();
    const auto task = metadata.value("pipeline_tag").toString();
    const auto config = metadata.value("config").toObject();
    const auto card = metadata.value("cardData").toObject();
    const auto architecture = metadata.value("gguf").toObject().value("architecture").toString();
    const bool audioCpp = architecture == "audiocpp" || metadata.value("library_name").toString() == "audio.cpp";
    const bool whisper = config.value("model_type").toString() == "whisper"
        || id.contains("whisper", Qt::CaseInsensitive);
    const auto declaredLanguages = languageIds(card.value("language"));
    const auto siblings = metadata.value("siblings").toArray();
    const auto tags = metadata.value("tags").toArray();
    QVariantMap result{{"id", id}, {"name", id.section('/', -1)}, {"revision", metadata.value("sha").toString()},
        {"kind", kind}, {"gated", gatedRepository(metadata)}};
    if (kind == "output" && task == "text-to-speech" && tags.contains("gradium_phonon_tts")) {
        auto variant = phononVariant(siblings);
        if (!variant.isEmpty()) {
            if (!declaredLanguages.isEmpty()) variant["languageNames"] = languageNames(declaredLanguages);
            variant["streaming"] = true;
            result["variants"] = QVariantList{variant};
        }
        return result;
    }
    QVariantList variants, otherAudioCpp;
    static const QRegularExpression unsupportedOutput("-[0-9]{5}-of-[0-9]{5}\\.gguf$", QRegularExpression::CaseInsensitiveOption);
    for (const auto &value : siblings) {
        auto file = audioFile(value.toObject());
        if (file.isEmpty()) continue;
        const auto path = file.value("path").toString();
        const auto basename = QFileInfo(path).fileName().toLower();
        const auto size = file.value("size").toLongLong();

        QString engine;
        QStringList languages = declaredLanguages;
        const QString format = kind == "input" ? "whisper-ggml" : "audio-gguf";
        if (kind == "input") {
            if (!basename.endsWith(".bin") || (!task.isEmpty() && task != "automatic-speech-recognition")
                || basename.startsWith("pytorch_model") || basename.contains("silero")
                || (!whisper && !basename.startsWith("ggml-") && task != "automatic-speech-recognition")) continue;
            engine = "whisper";
            if (basename.endsWith(".en.bin") || basename.contains(".en-")) languages = {"en"};
        } else {
            if (!basename.endsWith(".gguf") || basename.contains("mmproj") || unsupportedOutput.match(basename).hasMatch()) continue;
            engine = outputEngine(path);
            if (engine.isEmpty()) engine = outputEngine(id);
            if (engine.isEmpty() && !audioCpp && (task != "text-to-speech" || !architecture.isEmpty())) continue;
        }
        file["format"] = format;
        QVariantMap variant{{"name", path + ", " + QLocale().formattedDataSize(size)}, {"filename", path}, {"bytes", size},
            {"format", format}, {"engine", engine}, {"files", QVariantList{file}}};
        if (!languages.isEmpty()) variant["languageNames"] = languageNames(languages);
        if (card.value("streaming").isBool()) variant["streaming"] = card.value("streaming").toBool();
        (engine.isEmpty() ? otherAudioCpp : variants).append(variant);
    }
    // A shared audio.cpp repository can contain many engines this app cannot load.
    if (variants.isEmpty()) variants = otherAudioCpp;
    result["variants"] = variants;
    return result;
}

bool safeAudioSpec(const QJsonObject &spec)
{
    bool hasGguf = false;
    for (const auto &value : spec.value("sources").toArray()) {
        const auto source = value.toObject();
        const auto roots = source.value("roots").toObject();
        const bool gguf = source.value("format").toString() == "gguf";
        if (!gguf && source.value("format").toString() != "safetensors") return false;
        hasGguf |= gguf;
        for (const auto &value : roots) {
            const auto root = value.toString();
            if (root != "." && root != "$gguf" && !safePath(root)) return false;
        }
        for (const auto &key : {"files", "optional_files", "tensors", "optional_tensors"}) {
            const auto resources = source.value(key).toObject();
            for (const auto &value : resources) {
                const auto reference = value.isObject() ? value.toObject().value("source").toString() : value.toString();
                const auto split = reference.indexOf(':');
                if (split < 1 || !roots.contains(reference.first(split))) return false;
                const auto path = reference.sliced(split + 1);
                if (!path.isEmpty() && !safePath(path)) return false;
            }
        }
    }
    return hasGguf;
}

std::optional<QByteArray> readGgufString(QDataStream &stream, QBuffer &buffer)
{
    quint64 size = 0;
    stream >> size;
    if (stream.status() != QDataStream::Ok || size > quint64(buffer.bytesAvailable())) return std::nullopt;
    return buffer.read(qint64(size));
}

bool skipGgufValue(QDataStream &stream, QBuffer &buffer, quint32 type)
{
    constexpr quint32 string = 8, array = 9;
    constexpr std::array widths{1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};
    quint64 count = 1;
    if (type == array) stream >> type >> count;
    if (stream.status() != QDataStream::Ok || type >= widths.size() || type == array) return false;
    if (type == string) {
        while (count-- > 0)
            if (!readGgufString(stream, buffer)) return false;
        return true;
    }
    const auto width = widths[type];
    return count <= quint64(buffer.bytesAvailable() / width) && buffer.seek(buffer.pos() + qint64(count * width));
}

// The audio.cpp exporter puts its model spec before the large embedded assets.
// A missing result needs more bytes. An empty map means unsupported or invalid.
std::optional<QVariantMap> ggufAudioHeader(const QByteArray &prefix, const QString &engine)
{
    if (prefix.size() < 24) return std::nullopt;
    if (!prefix.startsWith("GGUF")) return QVariantMap{};
    QBuffer buffer;
    buffer.setData(prefix);
    buffer.open(QIODevice::ReadOnly);
    buffer.seek(4);
    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint32 version = 0;
    quint64 tensors = 0, fields = 0;
    stream >> version >> tensors >> fields;
    if (version != 3 || !tensors) return QVariantMap{};

    QByteArray architecture, family;
    quint32 specVersion = 0;
    QJsonObject spec;
    for (quint64 i = 0; i < fields; ++i) {
        const auto key = readGgufString(stream, buffer);
        if (!key) return std::nullopt;
        quint32 type = 0;
        stream >> type;
        if (stream.status() != QDataStream::Ok) return std::nullopt;
        if (*key == "audiocpp.model_spec.version") {
            if (type != 4) return QVariantMap{};
            stream >> specVersion;
            if (stream.status() != QDataStream::Ok) return std::nullopt;
            if (specVersion != 1) return QVariantMap{};
        } else if (*key == "general.architecture" || *key == "audiocpp.model_spec.family"
                   || *key == "audiocpp.model_spec.json") {
            if (type != 8) return QVariantMap{};
            const auto value = readGgufString(stream, buffer);
            if (!value) return std::nullopt;
            if (*key == "general.architecture") {
                architecture = *value;
                if (architecture != "audiocpp") return QVariantMap{};
            } else if (*key == "audiocpp.model_spec.family") {
                family = *value;
                if ((family != "supertonic" && family != "breeze_tts") || (!engine.isEmpty() && family != engine.toUtf8()))
                    return QVariantMap{};
            } else {
                spec = QJsonDocument::fromJson(*value).object();
                if ((spec.contains("schema_version") && spec.value("schema_version").toInt() != 1)
                    || !safeAudioSpec(spec)) return QVariantMap{};
            }
        } else if (!skipGgufValue(stream, buffer, type)) {
            return std::nullopt;
        }
        if (architecture.isEmpty() || family.isEmpty() || specVersion != 1 || spec.isEmpty()) continue;
        const auto verifiedEngine = QString::fromUtf8(family);
        if (spec.value("family").toString() != verifiedEngine) return QVariantMap{};
        const auto modes = spec.value("modes").toArray();
        if (!modes.contains("offline")) return QVariantMap{};
        QVariantMap result{{"engine", verifiedEngine}, {"streaming", modes.contains("streaming")}};
        auto languages = languageIds(spec.value("languages"));
        if (verifiedEngine == "supertonic") {
            if (spec.contains("languages")) {
                for (auto &id : languages) id = id.section('-', 0, 0);
                languages.removeIf([](const auto &id) { return id != "na" && !supertonicLanguages.contains(id); });
                languages.removeDuplicates();
                if (languages.isEmpty()) return QVariantMap{};
                result["languageIds"] = languages;
            }
            const auto presets = embeddedSupertonicPresets(spec);
            if (presets.isEmpty()) return QVariantMap{};
            result["presetIds"] = presets;
        }
        if (!languages.isEmpty()) result["languageNames"] = languageNames(languages, verifiedEngine == "supertonic");
        return result;
    }
    return QVariantMap{};
}

std::optional<QVariantMap> whisperHeader(const QByteArray &prefix, qint64 fileSize, bool hasDeclaredLanguages)
{
    if (prefix.size() < 56) return std::nullopt;
    if (!prefix.startsWith("lmgg")) return QVariantMap{};
    const auto field = [&](int index) { return qFromLittleEndian<quint32>(prefix.constData() + 4 + index * 4); };
    const auto vocab = field(0), audioState = field(2), textState = field(6), mels = field(9);
    if (vocab < 51864 || vocab > 51900 || audioState < 384 || audioState > 1280 || textState != audioState
        || (mels != 80 && mels != 128) || field(11) != mels || field(12) != 201
        || fileSize <= 56 + qint64(mels) * 201 * 4) return QVariantMap{};
    for (const int index : {1, 3, 4, 5, 7, 8})
        if (field(index) == 0 || field(index) > 1500) return QVariantMap{};
    QVariantMap result{{"engine", "whisper"}};
    if (vocab == 51864) result["languageNames"] = languageNames({"en"});
    else if (!hasDeclaredLanguages) result["languageNames"] = languageNames({"multilingual"});
    return result;
}

bool validPhononAsset(const QString &name, const QByteArray &prefix, qint64 size)
{
    if (name == "config.json") {
        const auto config = QJsonDocument::fromJson(prefix).object();
        return config.value("flow_lm").isObject() && config.value("mimi").isObject();
    }
    if (name == "tokenizer.model") return true;
    if (prefix.size() < 9 || prefix[8] != '{') return false;
    const auto length = qFromLittleEndian<quint64>(prefix.constData());
    return length > 2 && length < quint64(size - 8);
}

QNetworkRequest requestFor(const QUrl &url, const QByteArray &accessToken = {})
{
    QNetworkRequest request(url);
    request.setTransferTimeout(30000);
    request.setRawHeader("User-Agent", "Pilon/1.0");
    if (!accessToken.isEmpty() && url.scheme() == "https" && url.host() == "huggingface.co" && url.port(443) == 443) {
        request.setRawHeader("Authorization", "Bearer " + accessToken);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    }
    return request;
}

bool validHubToken(const QByteArray &token)
{
    return !token.isEmpty() && token.size() <= 1024
        && std::all_of(token.begin(), token.end(), [](unsigned char ch) { return ch > 32 && ch < 127; });
}

bool validHubAccount(const QString &name)
{
    static const QRegularExpression pattern("^[a-zA-Z0-9_-]{1,128}$");
    return pattern.match(name).hasMatch();
}

template <typename Work, typename Done>
void runHubCredentialOperation(QObject *owner, Work work, Done done)
{
    auto *watcher = new QFutureWatcher<AssistantHubCredentials::Result>(owner);
    QObject::connect(watcher, &QFutureWatcherBase::finished, owner, [watcher, done] {
        const auto result = watcher->result();
        watcher->deleteLater();
        done(result);
    });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}

QVariantMap reasoning(const QString &id, const QString &name)
{
    return {{"id", id}, {"name", name}};
}

QVariantMap capabilities(const QJsonObject &metadata)
{
    const auto gguf = metadata.value("gguf").toObject();
    const auto config = metadata.value("config").toObject();
    const QString chat = gguf.value("chat_template").toString(config.value("chat_template_jinja").toString());
    const QString task = metadata.value("pipeline_tag").toString();
    const bool vision = config.contains("vision_config") || chat.contains("image_url")
        || chat.contains("<|image") || task == "image-text-to-text";
    const bool audio = config.contains("audio_config") || chat.contains("input_audio") || chat.contains("<|audio|>");
    const bool video = chat.contains("video") && vision;
    QStringList modalities{QObject::tr("Text")};
    if (vision) modalities << QObject::tr("Image");
    if (audio) modalities << QObject::tr("Audio");
    if (video) modalities << QObject::tr("Video");

    QVariantList modes;
    const bool toggle = chat.contains("enable_thinking") || chat.contains("<|think|>");
    const bool thinks = toggle || chat.contains("<think>") || chat.contains("<|channel>thought")
        || chat.contains("reasoning_effort");
    if (toggle || !thinks) modes.append(reasoning("none", QObject::tr("Off")));
    if (chat.contains("reasoning_effort")) {
        // Prefer the template's explicit allow-list; aliases (e.g. high -> xhigh)
        // are not additional levels the model can distinguish.
        const QRegularExpression allowed("reasoning_effort\\s+(?:not\\s+)?in\\s*[\\[(]([^\\])]+)[\\])]");
        const auto match = allowed.match(chat);
        const QString values = match.hasMatch() ? match.captured(1) : chat;
        for (const auto &id : {QString("low"), QString("medium"), QString("high"), QString("xhigh")}) {
            if (values.contains("'" + id + "'") || values.contains('"' + id + '"'))
                modes.append(reasoning(id, id == "xhigh" ? QObject::tr("Maximum")
                    : id == "low" ? QObject::tr("Low") : id == "medium" ? QObject::tr("Medium") : QObject::tr("High")));
        }
    }
    if (thinks && (modes.isEmpty() || modes.last().toMap().value("id") == "none"))
        modes.append(reasoning("on", QObject::tr("On")));
    return {{"modalities", modalities}, {"vision", vision}, {"audio", audio}, {"video", video},
        {"reasoning", modes}, {"capabilitiesKnown", !chat.isEmpty()},
        {"context", gguf.value("context_length").toInt(8192)},
        {"architecture", gguf.value("architecture").toString(config.value("model_type").toString())}};
}
}

AssistantModels::AssistantModels(QObject *parent) : AssistantModels(Owner::Assistant, parent) {}
AssistantModels::AssistantModels(Owner owner, QObject *parent) : QObject(parent), m_owner(owner)
{
    if (m_owner == Owner::Assistant) refresh();
}
AssistantModels::~AssistantModels()
{
    if (m_inspectReply) { m_inspectReply->disconnect(this); m_inspectReply->abort(); }
    cancelDownload();
}

QString AssistantModels::root()
{
    const auto override = qEnvironmentVariable("PILON_MODELS_DIR");
    return override.isEmpty() ? PilonWorkspace::modelsDirectory() : override;
}

QString AssistantModels::handwritingRoot()
{
    const auto override = qEnvironmentVariable("PILON_NOTES_MODELS_DIR");
    if (!override.isEmpty()) return override;
    const auto legacyTestRoot = qEnvironmentVariable("PILON_NOTES_OCR_ROOT");
    if (!legacyTestRoot.isEmpty()) return QDir(legacyTestRoot).filePath("models");
    return PilonWorkspace::addonModelsDirectory("notes");
}

QString AssistantModels::modelDirectory() const
{
    return m_owner == Owner::Notes ? handwritingRoot() : root();
}

QString AssistantModels::prefixCacheDirectory(const QString &id)
{
    const auto directory = qEnvironmentVariable("PILON_PREFIX_CACHE_DIR",
        PilonWorkspace::appDataDirectory() + "/assistant/prefixes");
    return directory + '/' + QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex());
}

void AssistantModels::preparationFailed(const QString &message)
{
    m_error = message; emit stateChanged();
}

void AssistantModels::refresh()
{
    if (m_owner == Owner::Notes) return;
    QVariantList installed, audio;
    const auto add = [&](QVariantMap model) {
        const QString weights = model.value("weights").toString();
        const auto kind = model.value("kind").toString();
        const bool imported = kind == "input" || kind == "output";
        if ((imported ? !safeAudioPath(root(), weights) : !safePath(weights))
            || !QFileInfo(root() + '/' + weights).isFile()) return;
        qint64 bytes = 0;
        auto files = model.value("files").toList();
        if (files.isEmpty()) files.append(QVariantMap{{"path", weights}});
        for (const auto &value : files) {
            const auto file = value.toMap();
            const auto path = file.value("path").toString();
            if (imported ? !safeAudioPath(root(), path) : !safePath(path)) return;
            const QFileInfo info(root() + '/' + path);
            if (!info.isFile() || (file.contains("size") && info.size() != file.value("size").toLongLong())) return;
            bytes += info.size();
        }
        model["bytes"] = bytes;
        model["canonicalName"] = canonicalName(model.value("name").toString());
        const auto nickname = model.value("nickname").toString().trimmed();
        model["displayName"] = nickname.isEmpty() ? model.value("canonicalName").toString() : nickname;
        if (imported) {
            const auto engine = model.value("engine").toString();
            if (kind == "input" ? engine != "whisper"
                                : (engine != "supertonic" && engine != "breeze_tts" && engine != "gradium_phonon")) return;
            model.insert(speechSettings(engine, model));
            model["weights"] = QFileInfo(root() + '/' + weights).absoluteFilePath();
            model["imported"] = true;
            model["installed"] = true;
            audio.append(model);
        } else {
            installed.append(model);
        }
    };
    QFile manifest(root() + "/installed.json");
    if (manifest.open(QIODevice::ReadOnly))
        for (const auto &value : QJsonDocument::fromJson(manifest.readAll()).array()) add(value.toObject().toVariantMap());
    if (m_installed == installed && m_importedAudio == audio) return;
    m_installed = installed;
    m_importedAudio = audio;
    emit installedChanged();
}

QVariantList AssistantModels::supportModels() const
{
    if (m_owner == Owner::Assistant) return m_importedAudio;
    QFile source(":/qt/qml/Pilon/assistant-models.json");
    if (!source.open(QIODevice::ReadOnly)) return {};
    const auto files = QJsonDocument::fromJson(source.readAll()).object().value("files").toArray();
    const auto directory = modelDirectory();
    QVariantList selected;
    qint64 bytes = 0;
    bool installed = true;
    for (const auto &value : files) {
        const auto file = value.toObject().toVariantMap();
        const auto path = file.value("path").toString();
        if (!path.startsWith("ppocrv5/")) continue;
        const auto size = file.value("bytes").toLongLong();
        selected.append(file);
        bytes += size;
        const QFileInfo info(directory + '/' + path);
        installed = installed && info.isFile() && info.size() == size;
    }
    if (selected.isEmpty()) return {};
    return {QVariantMap{{"id", "ppocrv5"}, {"name", tr("Handwriting recognition")}, {"bytes", bytes},
        {"installed", installed}, {"files", selected}}};
}

void AssistantModels::downloadSupport(const QString &id)
{
    if (m_owner != Owner::Notes || m_downloading || m_inspecting) return;
    for (const auto &value : supportModels()) {
        const auto model = value.toMap();
        if (model.value("id") != id || model.value("installed").toBool()) continue;
        m_downloadFiles.clear(); QVariantList targets;
        for (const auto &item : model.value("files").toList()) {
            auto file = item.toMap();
            file.insert("size", file.value("bytes"));
            file.insert("format", "support");
            m_downloadFiles.append(file); targets.append(QVariantMap{{"path", file.value("path")}});
        }
        m_pendingModel = {{"id", id}, {"kind", "support"}, {"files", targets}};
        m_fileIndex = 0; m_completed = m_received = 0; m_total = model.value("bytes").toLongLong();
        m_error.clear(); m_downloading = true; emit stateChanged(); downloadNext(); return;
    }
}

bool AssistantModels::setNickname(const QString &id, const QString &nickname)
{
    QFile existing(root() + "/installed.json");
    QJsonParseError error;
    const auto document = existing.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(existing.readAll(), &error) : QJsonDocument{};
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        m_error = tr("The installed-model registry could not be read."); emit stateChanged(); return false;
    }
    existing.close();
    auto models = document.array();
    bool found = false;
    for (qsizetype i = 0; i < models.size(); ++i) {
        auto model = models[i].toObject();
        if (model.value("id").toString() != id) continue;
        if (nickname.trimmed().isEmpty()) model.remove("nickname");
        else model.insert("nickname", nickname.trimmed());
        models[i] = model; found = true; break;
    }
    if (!found) { m_error = tr("This model is no longer installed."); emit stateChanged(); return false; }
    QSaveFile manifest(root() + "/installed.json");
    const auto data = QJsonDocument(models).toJson();
    if (!manifest.open(QIODevice::WriteOnly) || manifest.write(data) != data.size() || !manifest.commit()) {
        m_error = tr("Unable to save the model nickname: %1").arg(manifest.errorString()); emit stateChanged(); return false;
    }
    m_error.clear(); refresh(); emit stateChanged(); return true;
}

bool AssistantModels::setContextSize(const QString &id, int tokens)
{
    if (tokens < 2048 || tokens > 2097152) {
        m_error = tr("Choose a context size between 2,048 and 2,097,152 tokens."); emit stateChanged(); return false;
    }
    QFile existing(root() + "/installed.json");
    if (!existing.open(QIODevice::ReadOnly)) { m_error = existing.errorString(); emit stateChanged(); return false; }
    const auto document = QJsonDocument::fromJson(existing.readAll()); existing.close();
    if (!document.isArray()) { m_error = tr("Unable to read the installed-model registry."); emit stateChanged(); return false; }
    auto models = document.array(); bool found = false;
    for (qsizetype i = 0; i < models.size(); ++i) {
        auto model = models[i].toObject();
        if (model.value("id").toString() != id) continue;
        if (model.value("context").toInt(8192) == tokens) return true;
        model.insert("context", tokens); models[i] = model; found = true; break;
    }
    if (!found) { m_error = tr("This model is no longer installed."); emit stateChanged(); return false; }
    QSaveFile file(root() + "/installed.json"); const auto bytes = QJsonDocument(models).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        m_error = tr("Unable to save context size: %1").arg(file.errorString()); emit stateChanged(); return false;
    }
    m_error.clear(); refresh(); emit stateChanged(); emit contextSizeChanged(id); return true;
}

bool AssistantModels::removeModel(const QString &id)
{
    if (m_downloading) return false;
    const auto fail = [this](const QString &message) {
        m_error = message; emit stateChanged(); return false;
    };
    QFile existing(root() + "/installed.json");
    QJsonParseError error;
    const auto document = existing.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(existing.readAll(), &error) : QJsonDocument{};
    if (error.error != QJsonParseError::NoError || !document.isArray())
        return fail(tr("The installed-model registry could not be read."));
    existing.close();
    const auto paths = [](const QJsonObject &model) {
        QSet<QString> result;
        for (const auto &file : model.value("files").toArray())
            result.insert(file.toObject().value("path").toString());
        result.insert(model.value("weights").toString());
        result.insert(model.value("projector").toString());
        result.remove(QString{});
        return result;
    };
    QJsonArray remaining;
    QSet<QString> removedFiles, retainedFiles;
    bool found = false;
    for (const auto &value : document.array()) {
        const auto model = value.toObject();
        if (model.value("id").toString() == id) { removedFiles.unite(paths(model)); found = true; }
        else { remaining.append(value); retainedFiles.unite(paths(model)); }
    }
    if (!found) return fail(tr("This model is no longer installed."));
    const auto directory = QFileInfo(root()).canonicalFilePath() + '/';
    QSet<QString> retainedTargets;
    for (const auto &path : retainedFiles)
        retainedTargets.insert(QFileInfo(root() + '/' + path).canonicalFilePath());
    QStringList files;
    for (const auto &path : removedFiles) {
        if (!safePath(path)) return fail(tr("The model registry contains an unsafe file path."));
        if (retainedFiles.contains(path)) continue;
        const QFileInfo file(root() + '/' + path);
        if (!file.exists() && !file.isSymLink()) continue;
        if (!file.isFile() || !file.canonicalFilePath().startsWith(directory))
            return fail(tr("The model registry contains a file outside the models directory."));
        if (!retainedTargets.contains(file.canonicalFilePath())) files.append(file.absoluteFilePath());
    }
    QSaveFile manifest(root() + "/installed.json");
    const auto data = QJsonDocument(remaining).toJson();
    if (!manifest.open(QIODevice::WriteOnly) || manifest.write(data) != data.size() || !manifest.commit())
        return fail(tr("Unable to remove the model: %1").arg(manifest.errorString()));
    // Commit the registry first, so a failed save cannot leave registered models
    // pointing at deleted files. Never recursively remove a shared model folder.
    QStringList failed;
    for (const auto &file : files)
        if (!QFile::remove(file)) failed.append(QFileInfo(file).fileName());
    const auto cache = prefixCacheDirectory(id);
    if (QFileInfo::exists(cache) && !QDir(cache).removeRecursively()) failed.append(tr("instruction caches"));
    m_error = failed.isEmpty() ? QString{} : tr("Model removed, but these files could not be deleted: %1").arg(failed.join(", "));
    m_status.clear(); refresh(); emit stateChanged(); return true;
}

QString AssistantModels::canonicalName(const QString &name)
{
    QString base = name.section('/', -1).trimmed();
    base.remove(QRegularExpression("\\.(?:gguf|litertlm)$", QRegularExpression::CaseInsensitiveOption));
    base.remove(QRegularExpression("-[0-9]{5}-of-[0-9]{5}$"));
    const QRegularExpression quantization("(?:[\\s_\\-·]+)((?:UD[-_])?(?:I?Q[0-9]+(?:_[A-Za-z0-9]+)*|BF16|F16|F32|FP16|FP32))$",
                                         QRegularExpression::CaseInsensitiveOption);
    const auto quant = quantization.match(base);
    const QString precision = quant.hasMatch() ? quant.captured(1).toUpper() : QString{};
    if (quant.hasMatch()) base.truncate(quant.capturedStart());
    const QRegularExpression size("(?:^|[-_\\s])((?:E)?[0-9]+(?:\\.[0-9]+)?(?:x[0-9]+)?[BM](?:[-_]A[0-9]+(?:\\.[0-9]+)?[BM])?)(?=$|[-_\\s])",
                                  QRegularExpression::CaseInsensitiveOption);
    const auto parameters = size.match(base);
    // Keep unfamiliar names intact rather than guessing their family or size.
    if (!parameters.hasMatch()) return name;
    QString family = base.first(parameters.capturedStart());
    family.remove(QRegularExpression("^google[_-]", QRegularExpression::CaseInsensitiveOption));
    family.replace(QRegularExpression("[-_]+"), " ");
    family = family.simplified();
    if (family.isEmpty()) return name;
    family[0] = family[0].toUpper();
    return family + ' ' + parameters.captured(1).toUpper() + (precision.isEmpty() ? QString{} : ' ' + precision);
}

QString AssistantModels::repositoryId(const QString &link)
{
    QString id = link.trimmed();
    if (id.startsWith("https://")) {
        const QUrl url(id);
        if (url.host() != "huggingface.co" && url.host() != "hf.co") return {};
        if (!url.userInfo().isEmpty() || url.port() != -1) return {};
        const auto parts = url.path().split('/', Qt::SkipEmptyParts);
        if (parts.size() < 2 || (parts.size() > 2 && parts[2] != "tree" && parts[2] != "blob")) return {};
        id = parts[0] + '/' + parts[1];
    }
    static const QRegularExpression valid("^[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*$");
    return valid.match(id).hasMatch() && !id.contains("..") ? id : QString{};
}

QVariantMap AssistantModels::describeRepository(const QJsonObject &metadata)
{
    auto result = capabilities(metadata);
    result["gated"] = gatedRepository(metadata);
    result["id"] = metadata.value("id").toString();
    result["name"] = metadata.value("id").toString().section('/', -1);
    result["revision"] = metadata.value("sha").toString();
    QMap<QString, QVariantList> groups;
    QVariantList projectors;
    const QRegularExpression shard("^(.*)-([0-9]{5})-of-([0-9]{5})\\.gguf$", QRegularExpression::CaseInsensitiveOption);
    for (const auto &value : metadata.value("siblings").toArray()) {
        const auto item = value.toObject(); const auto name = item.value("rfilename").toString();
        const bool liteRt = name.endsWith(".litertlm", Qt::CaseInsensitive);
        if (!safePath(name) || (!liteRt && !name.endsWith(".gguf", Qt::CaseInsensitive))) continue;
        const auto lfs = item.value("lfs").toObject();
        const qint64 size = item.value("size").toInteger(lfs.value("size").toInteger());
        if (size <= 0) continue;
        QVariantMap file{{"path", name}, {"size", size}, {"sha256", lfs.value("sha256").toString()},
            {"format", liteRt ? "litertlm" : "gguf"}};
        const auto basename = QFileInfo(name).fileName().toLower();
        if (!liteRt && basename.contains("mmproj")) { file["name"] = name; projectors.append(file); continue; }
        if (!liteRt && (basename.startsWith("mtp-") || basename.contains("imatrix"))) continue;
        const auto match = shard.match(name);
        const auto key = match.hasMatch() ? match.captured(1) + ".gguf" : name;
        if (match.hasMatch()) { file["shard"] = match.captured(2).toInt(); file["shards"] = match.captured(3).toInt(); }
        groups[key].append(file);
    }
    QVariantList variants;
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        auto files = it.value();
        std::sort(files.begin(), files.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap().value("path").toString() < b.toMap().value("path").toString();
        });
        bool complete = true; qint64 bytes = 0;
        for (int i = 0; i < files.size(); ++i) {
            const auto file = files[i].toMap(); bytes += file.value("size").toLongLong();
            if (file.contains("shards")) complete &= file.value("shards").toInt() == files.size() && file.value("shard").toInt() == i + 1;
        }
        if (complete) variants.append(QVariantMap{{"name", it.key() + ", " + QString::number(bytes / double(GiB), 'f', 1) + " GiB"},
            {"filename", it.key()}, {"format", files.first().toMap().value("format")}, {"files", files}, {"bytes", bytes}});
    }
    result["variants"] = variants; result["projectors"] = projectors;
    return result;
}

bool AssistantModels::hubUsesEnvironmentToken() const
{
    return m_hubToken.isEmpty() && validHubToken(qgetenv("HF_TOKEN"));
}

QByteArray AssistantModels::hubToken() const
{
    if (!m_hubToken.isEmpty()) return m_hubToken;
    const auto environment = qgetenv("HF_TOKEN");
    return validHubToken(environment) ? environment : QByteArray{};
}

void AssistantModels::finishHubAuth(const QString &error)
{
    m_hubAuthBusy = false;
    m_hubAuthError = error;
    const auto link = std::exchange(m_pendingHubLink, {});
    const auto kind = std::exchange(m_pendingHubAudioKind, {});
    emit hubAuthChanged();
    if (!link.isEmpty()) inspectRepository(link, kind);
}

void AssistantModels::loadHubAccount()
{
    if (m_owner != Owner::Assistant || m_hubAuthBusy
        || (m_hubCredentialsLoaded && (m_hubAuthError.isEmpty() || !m_hubToken.isEmpty()))) return;
    m_hubAuthBusy = true;
    emit hubAuthChanged();
    runHubCredentialOperation(this, AssistantHubCredentials::lookup, [this](const auto &result) {
        m_hubCredentialsLoaded = true;
        if (!result.ok || result.token.isEmpty()) { finishHubAuth(result.error); return; }
        const auto saved = QJsonDocument::fromJson(result.token).object();
        const auto token = saved.value("token").toString().toUtf8();
        const auto name = saved.value("account").toString();
        if (!validHubToken(token) || !validHubAccount(name)) {
            finishHubAuth(tr("The saved Hugging Face sign-in could not be read. Sign in again."));
            return;
        }
        m_hubToken = token;
        m_hubAccount = name;
        finishHubAuth();
    });
}

void AssistantModels::signInHub(const QString &accessToken)
{
    if (m_owner != Owner::Assistant || m_hubAuthBusy || m_downloading) return;
    const auto token = accessToken.trimmed().toUtf8();
    if (!validHubToken(token)) { finishHubAuth(tr("Enter a valid Hugging Face access token.")); return; }
    m_hubAuthBusy = true;
    m_hubAuthError.clear();
    emit hubAuthChanged();
    auto url = m_hub;
    url.setPath("/api/whoami-v2");
    auto *reply = m_network.get(requestFor(url, token));
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > 1024 * 1024) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, token] {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        const auto account = QJsonDocument::fromJson(reply->readAll()).object().value("name").toString();
        reply->deleteLater();
        if (status == 401 || status == 403) {
            finishHubAuth(tr("Hugging Face rejected this access token. Check it and try again."));
            return;
        }
        if (error != QNetworkReply::NoError || !validHubAccount(account)) {
            finishHubAuth(tr("Unable to verify the Hugging Face account. Try again."));
            return;
        }
        const auto secret = QJsonDocument(QJsonObject{{"account", account}, {"token", QString::fromLatin1(token)}})
            .toJson(QJsonDocument::Compact);
        runHubCredentialOperation(this, [secret] { return AssistantHubCredentials::store(secret); },
            [this, token, account](const auto &result) {
                if (result.ok) {
                    m_hubToken = token;
                    m_hubAccount = account;
                    m_hubCredentialsLoaded = true;
                }
                finishHubAuth(result.error);
            });
    });
}

void AssistantModels::signOutHub()
{
    if (m_owner != Owner::Assistant || m_hubAuthBusy || m_hubToken.isEmpty()) return;
    if (m_inspecting || m_downloading) {
        finishHubAuth(tr("Finish or cancel the model import before signing out."));
        return;
    }
    m_hubAuthBusy = true;
    m_hubAuthError.clear();
    emit hubAuthChanged();
    runHubCredentialOperation(this, AssistantHubCredentials::clear, [this](const auto &result) {
        if (result.ok) {
            m_hubToken.clear();
            m_hubAccount.clear();
            m_repository.clear();
            m_audioRepository.clear();
            emit repositoryChanged();
            emit audioRepositoryChanged();
        }
        finishHubAuth(result.error);
    });
}

void AssistantModels::inspect(const QString &link)
{
    inspectRepository(link, {});
}

void AssistantModels::inspectAudio(const QString &link, const QString &kind)
{
    if (m_owner != Owner::Assistant || (kind != "input" && kind != "output")) return;
    inspectRepository(link, kind);
}

void AssistantModels::inspectRepository(const QString &link, const QString &audioKind)
{
    if (m_downloading) return;
    m_pendingHubLink.clear();
    m_pendingHubAudioKind.clear();
    const bool audio = !audioKind.isEmpty();
    const auto repository = audio ? &AssistantModels::m_audioRepository : &AssistantModels::m_repository;
    const auto changed = audio ? &AssistantModels::audioRepositoryChanged : &AssistantModels::repositoryChanged;
    if (m_inspectReply) { m_inspectReply->disconnect(this); m_inspectReply->abort(); m_inspectReply->deleteLater(); m_inspectReply = nullptr; }
    (this->*repository).clear(); m_error.clear(); m_status.clear(); m_inspecting = false;
    emit (this->*changed)();
    const auto id = repositoryId(link);
    if (id.isEmpty()) {
        if (!link.trimmed().isEmpty()) m_error = tr("Paste a Hugging Face model repository URL or owner/repository.");
        emit stateChanged(); return;
    }
    m_inspecting = true; m_status = tr("Reading model information…"); emit stateChanged();
    if (m_owner == Owner::Assistant && (!m_hubCredentialsLoaded || m_hubAuthBusy)) {
        m_pendingHubLink = id;
        m_pendingHubAudioKind = audioKind;
        loadHubAccount();
        return;
    }
    QUrl url = m_hub;
    url.setPath("/api/models/" + id); url.setQuery("blobs=true");
    m_inspectReply = m_network.get(requestFor(url, m_owner == Owner::Assistant ? hubToken() : QByteArray{}));
    connect(m_inspectReply, &QNetworkReply::readyRead, this, [this] {
        if (m_inspectReply && m_inspectReply->bytesAvailable() > 16 * 1024 * 1024) m_inspectReply->abort();
    });
    connect(m_inspectReply, &QNetworkReply::finished, this, [this, repository, changed, audioKind] {
        auto reply = m_inspectReply; m_inspectReply = nullptr; m_inspecting = false; m_status.clear();
        if (reply->error() != QNetworkReply::NoError) {
            const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            m_error = code == 401 || code == 403 ? tr("Sign in under Assistant → Hugging Face and check that your account can access this repository.")
                : tr("Unable to read the repository: %1").arg(reply->errorString());
        } else {
            const auto metadata = QJsonDocument::fromJson(reply->readAll()).object();
            if (repositoryId(metadata.value("id").toString()).isEmpty() || metadata.value("sha").toString().isEmpty())
                m_error = tr("The repository returned incomplete model information.");
            else {
                auto &description = this->*repository;
                description = audioKind.isEmpty() ? describeRepository(metadata) : describeAudioRepository(metadata, audioKind);
                if (description.value("variants").toList().isEmpty()) {
                    if (audioKind == "input") m_error = tr("This repository has no Whisper GGML (.bin) speech recognition model.");
                    else if (audioKind == "output") m_error = tr("This repository has no compatible speech output model. Choose an audio.cpp Supertonic or Breeze GGUF, or a complete Phonon checkpoint.");
                    else m_error = tr("This repository has no complete GGUF or LiteRT-LM (.litertlm) model files.");
                }
            }
        }
        reply->deleteLater(); emit (this->*changed)(); emit stateChanged();
    });
}

QString AssistantModels::memoryWarning(qint64 bytes, qint64 available, qint64 total)
{
    if (bytes <= 0 || total <= 0) return {};
    // A lower bound: weights plus runtime overhead. Context/KV cache and other
    // applications can require more; GPU fitting may spill into system RAM.
    const qint64 overhead = std::max(GiB * 2, bytes / 5);
    const qint64 required = bytes + std::min(overhead, std::numeric_limits<qint64>::max() - bytes);
    if (required <= available) return {};
    return required > total
        ? tr("This model may be too large for this hardware: approximately %1 GiB or more is needed; this device has %2 GiB RAM in total, with %3 GiB currently available. Context and GPU memory add to the requirement.")
            .arg(required / double(GiB), 0, 'f', 1).arg(total / double(GiB), 0, 'f', 1).arg(available / double(GiB), 0, 'f', 1)
        : tr("Low available memory for loading: approximately %1 GiB or more is needed; %2 GiB of %3 GiB RAM is currently available. Loading may be slow or fail, especially with a long context.")
            .arg(required / double(GiB), 0, 'f', 1).arg(available / double(GiB), 0, 'f', 1).arg(total / double(GiB), 0, 'f', 1);
}

QString AssistantModels::hardwareWarning(qint64 bytes) const
{
    QFile memory("/proc/meminfo"); qint64 available = 0, total = 0;
    if (memory.open(QIODevice::ReadOnly)) {
        const auto lines = memory.readAll().split('\n');
        for (const auto &line : lines) {
            const auto fields = line.simplified().split(' ');
            if (fields.size() < 2) continue;
            if (fields[0] == "MemTotal:") total = fields[1].toLongLong() * 1024;
            if (fields[0] == "MemAvailable:") available = fields[1].toLongLong() * 1024;
        }
    }
    return memoryWarning(bytes, available, total);
}

QVariantList AssistantModels::selectedFiles(const QVariantMap &repository, int variant, int projector)
{
    const auto variants = repository.value("variants").toList();
    if (variant < 0 || variant >= variants.size()) return {};
    auto files = variants[variant].toMap().value("files").toList();
    const auto projectors = repository.value("projectors").toList();
    if (variants[variant].toMap().value("format") != "litertlm" && projector >= 0 && projector < projectors.size())
        files.append(projectors[projector]);
    return files;
}

QString AssistantModels::downloadWarning(int variant, int projector) const
{
    return warningsForFiles(selectedFiles(m_repository, variant, projector));
}

QString AssistantModels::audioDownloadWarning(int variant) const
{
    return warningsForFiles(selectedFiles(m_audioRepository, variant, -1));
}

QString AssistantModels::warningsForFiles(const QVariantList &files) const
{
    qint64 bytes = 0;
    for (const auto &file : files) bytes += file.toMap().value("size").toLongLong();
    QString warning = hardwareWarning(bytes);
    QStorageInfo disk(QFileInfo(root()).exists() ? root() : QFileInfo(root()).absolutePath());
    if (disk.isValid() && disk.bytesAvailable() < bytes)
        warning += (warning.isEmpty() ? "" : "\n") + tr("Not enough free disk space for this download (%1 GiB).").arg(bytes / double(GiB), 0, 'f', 1);
    return warning;
}

QString AssistantModels::modelWarning(int index) const
{
    return hardwareWarning(m_installed.value(index).toMap().value("bytes").toLongLong());
}

double AssistantModels::progress() const { return m_total > 0 ? (m_completed + m_received) / double(m_total) : 0; }

void AssistantModels::download(int variant, int projector, const QString &nickname)
{
    startDownload(m_repository, variant, projector, nickname);
}

void AssistantModels::downloadAudio(int variant, const QString &nickname)
{
    if (m_owner == Owner::Assistant) startDownload(m_audioRepository, variant, -1, nickname);
}

void AssistantModels::startDownload(const QVariantMap &repository, int variant, int projector, const QString &nickname)
{
    if (m_inspecting || m_downloading) return;
    if (m_hubAuthBusy) { failDownload(tr("Wait for the Hugging Face account operation to finish.")); return; }
    if (projector < -1 || projector >= repository.value("projectors").toList().size()) {
        failDownload(tr("Choose a valid multimodal projector.")); return;
    }
    m_downloadFiles = selectedFiles(repository, variant, projector);
    if (m_downloadFiles.isEmpty()) return;
    m_total = 0; m_completed = 0; m_received = 0; m_fileIndex = 0; m_error.clear();
    for (const auto &file : m_downloadFiles) m_total += file.toMap().value("size").toLongLong();
    if (!QDir().mkpath(root())) { failDownload(tr("Unable to create the models directory.")); return; }
    QStorageInfo disk(root());
    if (disk.isValid() && disk.bytesAvailable() < m_total) { failDownload(tr("Not enough free disk space for this model.")); return; }
    m_pendingModel = repository;
    m_pendingModel.remove("variants"); m_pendingModel.remove("projectors"); m_pendingModel.remove("gated");
    const auto chosen = repository.value("variants").toList()[variant].toMap();
    const bool audio = repository.contains("kind");
    const bool liteRt = chosen.value("format") == "litertlm";
    if (liteRt) projector = -1;
    m_pendingModel["format"] = chosen.value("format", "gguf");
    const auto id = repository.value("id").toString();
    const auto revision = repository.value("revision").toString();
    static const QRegularExpression commit("^[a-fA-F0-9]{40,64}$");
    if (repositoryId(id).isEmpty() || !commit.match(revision).hasMatch()) { failDownload(tr("Invalid repository revision.")); return; }
    m_downloadRepository = id;
    const QString directory = (audio ? "hf-audio/" : "hf/") + id + '/' + revision + '/';
    const auto filename = chosen.value("filename").toString();
    m_pendingModel["id"] = audio ? "audio:" + id + '/' + revision + '/' + filename : id + '/' + filename;
    m_pendingModel["name"] = chosen.value("engine") == "gradium_phonon" ? repository.value("name").toString()
        : QFileInfo(filename).completeBaseName();
    if (!nickname.trimmed().isEmpty()) m_pendingModel["nickname"] = nickname.trimmed();
    m_pendingModel["weights"] = directory + m_downloadFiles.first().toMap().value("path").toString();
    if (audio) {
        m_pendingModel["engine"] = chosen.value("engine");
        for (const auto &key : {"languageNames", "streaming"})
            if (chosen.contains(key)) m_pendingModel[key] = chosen.value(key);
    } else {
        m_pendingModel["projector"] = projector >= 0 ? directory + m_downloadFiles.last().toMap().value("path").toString() : QString{};
        if (projector < 0 && !liteRt) {
            m_pendingModel["vision"] = false; m_pendingModel["audio"] = false; m_pendingModel["video"] = false;
            m_pendingModel["modalities"] = QStringList{tr("Text")};
        }
    }
    QVariantList files;
    for (const auto &value : m_downloadFiles) {
        auto file = value.toMap(); file["path"] = directory + file.value("path").toString(); files.append(file);
    }
    m_pendingModel["files"] = files;
    m_downloadToken = m_owner == Owner::Assistant ? hubToken() : QByteArray{};
    m_downloading = true; emit stateChanged(); downloadNext();
}

void AssistantModels::downloadNext()
{
    if (m_fileIndex == m_downloadFiles.size()) {
        if (m_pendingModel.value("kind") == "support") {
            m_downloading = false; m_status = tr("Model downloaded.");
            emit installedChanged(); emit stateChanged(); emit downloadCompleted(); return;
        }
        QFile existing(root() + "/installed.json"); QJsonArray models;
        const QFileInfo registry(existing.fileName());
        if (registry.isSymLink() || (registry.exists() && !existing.open(QIODevice::ReadOnly))) {
            failDownload(tr("The installed-model registry could not be read."));
            return;
        }
        if (existing.isOpen()) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(existing.readAll(), &error);
            if (existing.error() != QFileDevice::NoError
                || error.error != QJsonParseError::NoError || !document.isArray()) {
                failDownload(tr("The installed-model registry could not be read."));
                return;
            }
            existing.close();
            for (const auto &value : document.array())
                if (value.toObject().value("id").toString() != m_pendingModel.value("id").toString()) models.append(value);
        }
        models.append(QJsonObject::fromVariantMap(m_pendingModel));
        QSaveFile manifest(root() + "/installed.json"); const auto data = QJsonDocument(models).toJson();
        if (!manifest.open(QIODevice::WriteOnly) || manifest.write(data) != data.size() || !manifest.commit()) {
            failDownload(tr("Unable to save the installed-model registry: %1").arg(manifest.errorString())); return;
        }
        m_unregisteredAudioPaths.clear();
        m_downloadToken.clear();
        const auto id = m_pendingModel.value("id").toString();
        const bool audio = m_pendingModel.contains("engine");
        m_downloading = false; m_status = tr("Model downloaded."); refresh(); emit stateChanged();
        if (audio) emit audioModelDownloaded(id);
        else emit modelDownloaded(id);
        emit downloadCompleted(); return;
    }
    const auto file = m_downloadFiles[m_fileIndex].toMap();
    const auto target = m_pendingModel.value("files").toList()[m_fileIndex].toMap().value("path").toString();
    const auto path = modelDirectory() + '/' + target;
    const bool audio = m_pendingModel.contains("engine");
    const bool safe = audio ? safeAudioPath(modelDirectory(), target) : safePath(target);
    if (!safe || !QDir().mkpath(QFileInfo(path).absolutePath())) {
        failDownload(tr("Unable to create the model folder.")); return;
    }
    m_file = std::make_unique<QSaveFile>(path);
    if (!m_file->open(QIODevice::WriteOnly)) { failDownload(m_file->errorString()); return; }
    m_received = 0; m_hash.reset(); m_magic.clear(); m_audioHeader.clear();
    QUrl url = m_hub;
    url.setPath('/' + m_downloadRepository + "/resolve/" + m_pendingModel.value("revision").toString() + '/' + file.value("path").toString());
    if (file.contains("url")) url = QUrl(file.value("url").toString());
    m_status = tr("Downloading %1 (%2/%3)…").arg(file.value("path").toString()).arg(m_fileIndex + 1).arg(m_downloadFiles.size());
    fetchDownload(url);
    emit stateChanged();
}

void AssistantModels::fetchDownload(const QUrl &url, int redirects)
{
    auto request = requestFor(url, m_downloadToken);
    // Rebuild headers after each redirect so the Hub token stays on huggingface.co.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    m_downloadReply = m_network.get(request);
    m_downloadReply->setReadBufferSize(1024 * 1024);
    connect(m_downloadReply, &QNetworkReply::readyRead, this, &AssistantModels::readDownload);
    connect(m_downloadReply, &QNetworkReply::finished, this, [this, redirects] {
        const auto redirect = m_downloadReply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        if (!redirect.isEmpty()) {
            const auto source = m_downloadReply->url();
            const auto next = source.resolved(redirect);
            m_downloadReply->deleteLater();
            m_downloadReply = nullptr;
            if (redirects <= 0 || !next.isValid() || !next.userInfo().isEmpty()
                || (next.scheme() != "https" && !(source.scheme() == "http" && next.scheme() == "http"))) {
                failDownload(tr("The model download returned an unsafe or repeated redirect."));
                return;
            }
            fetchDownload(next, redirects - 1);
            return;
        }
        readDownload();
        if (!m_downloadReply) return;
        auto reply = m_downloadReply; m_downloadReply = nullptr;
        const auto error = reply->error(); const auto message = reply->errorString();
        const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        const bool audio = m_pendingModel.contains("engine");
        if (error != QNetworkReply::NoError) {
            failDownload(m_owner == Owner::Assistant && (code == 401 || code == 403)
                ? tr("Sign in under Assistant → Hugging Face. For a gated model, accept its terms on Hugging Face before downloading.")
                : tr("Download failed: %1").arg(message));
            return;
        }
        const auto file = m_downloadFiles[m_fileIndex].toMap();
        const auto expectedHash = file.value("sha256").toString().toLatin1();
        const auto format = file.value("format");
        const bool validMagic = format == "phonon-checkpoint" ? validPhononAsset(file.value("path").toString(), m_magic, m_received)
            : audio ? !m_audioHeader.isEmpty() : format == "support" || (format == "litertlm" ? m_magic == "LITERTLM" : m_magic.startsWith("GGUF"));
        if (!validMagic || m_received != file.value("size").toLongLong() || (!expectedHash.isEmpty() && m_hash.result().toHex() != expectedHash)) {
            failDownload(tr("The model download failed its integrity check. Please try again.")); return;
        }
        if (audio) m_pendingModel.insert(m_audioHeader);
        const auto path = m_file->fileName();
        const bool newAudioFile = audio && !QFileInfo::exists(path);
        if (!m_file->commit()) { failDownload(m_file->errorString()); return; }
        if (newAudioFile) m_unregisteredAudioPaths.append(path);
        m_file.reset(); m_completed += m_received; m_received = 0; ++m_fileIndex;
        if (!audio) emit stateChanged();
        downloadNext();
    });
}

void AssistantModels::readDownload()
{
    if (!m_downloadReply || !m_file || m_downloadReply->error() != QNetworkReply::NoError
        || m_downloadReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 400
        || m_downloadReply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid()) return;
    while (m_downloadReply->bytesAvailable()) {
        const auto bytes = m_downloadReply->read(256 * 1024);
        if (bytes.isEmpty()) break;
        const auto file = m_downloadFiles[m_fileIndex].toMap();
        const auto expectedSize = file.value("size").toLongLong();
        if (bytes.size() > expectedSize - m_received) {
            failDownload(tr("The download is larger than the repository's declared file size.")); return;
        }
        if (file.value("format") == "phonon-checkpoint") {
            const auto limit = file.value("path") == "config.json" ? audioHeaderLimit : qsizetype(9);
            if (m_magic.size() < limit) m_magic += bytes.first(std::min(limit - m_magic.size(), bytes.size()));
        } else if (!m_pendingModel.contains("engine")) {
            if (m_magic.size() < 8) m_magic += bytes.first(std::min(8 - m_magic.size(), bytes.size()));
        } else if (m_audioHeader.isEmpty()) {
            m_magic += bytes.first(std::min(audioHeaderLimit - m_magic.size(), bytes.size()));
            const auto header = file.value("format") == "whisper-ggml"
                ? whisperHeader(m_magic, expectedSize, m_pendingModel.contains("languageNames"))
                : ggufAudioHeader(m_magic, m_pendingModel.value("engine").toString());
            if (header && !header->isEmpty()) {
                m_audioHeader = *header;
                m_magic.clear();
            } else if (header || m_magic.size() == audioHeaderLimit) {
                failDownload(tr("The file is incompatible with Pilon’s local speech engines or has invalid model metadata.")); return;
            }
        }
        if (m_file->write(bytes) != bytes.size()) { failDownload(tr("Unable to write the model: %1").arg(m_file->errorString())); return; }
        m_hash.addData(bytes); m_received += bytes.size();
    }
    emit stateChanged();
}

void AssistantModels::cancelDownload()
{
    if (m_downloadReply) { m_downloadReply->disconnect(this); m_downloadReply->abort(); m_downloadReply->deleteLater(); m_downloadReply = nullptr; }
    if (m_file) { m_file->cancelWriting(); m_file.reset(); }
    for (const auto &path : m_unregisteredAudioPaths) QFile::remove(path);
    m_unregisteredAudioPaths.clear();
    m_downloadToken.clear();
    if (m_downloading) { m_downloading = false; m_status = tr("Download cancelled."); emit stateChanged(); }
}

void AssistantModels::failDownload(const QString &message)
{
    cancelDownload(); m_status.clear(); m_error = message; emit stateChanged();
}
