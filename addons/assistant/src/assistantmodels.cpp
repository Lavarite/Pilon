#include "../../../src/core/workspace/pilonworkspace.h"
#include "assistantmodels.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSet>
#include <QStorageInfo>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>

namespace {
constexpr qint64 GiB = 1024LL * 1024 * 1024;

bool safePath(const QString &path)
{
    return !path.isEmpty() && !QDir::isAbsolutePath(path) && !path.contains('\\')
        && !path.split('/').contains("..") && !path.split('/').contains(".")
        && !path.contains(QChar::Null);
}

QNetworkRequest requestFor(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(30000);
    request.setRawHeader("User-Agent", "Pilon/1.0");
    const auto token = qgetenv("HF_TOKEN");
    if (!token.isEmpty() && url.host() == "huggingface.co") request.setRawHeader("Authorization", "Bearer " + token);
    return request;
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

AssistantModels::AssistantModels(QObject *parent) : QObject(parent) { refresh(); }
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
    QVariantList installed;
    const auto add = [&](QVariantMap model) {
        const QString weights = model.value("weights").toString();
        if (!safePath(weights) || !QFileInfo(root() + '/' + weights).isFile()) return;
        qint64 bytes = 0;
        auto files = model.value("files").toList();
        if (files.isEmpty()) files.append(QVariantMap{{"path", weights}});
        for (const auto &value : files) {
            const auto file = value.toMap();
            const auto path = file.value("path").toString();
            if (!safePath(path)) return;
            const QFileInfo info(root() + '/' + path);
            if (!info.isFile() || (file.contains("size") && info.size() != file.value("size").toLongLong())) return;
            bytes += info.size();
        }
        model["bytes"] = bytes;
        model["canonicalName"] = canonicalName(model.value("name").toString());
        const auto nickname = model.value("nickname").toString().trimmed();
        model["displayName"] = nickname.isEmpty() ? model.value("canonicalName").toString() : nickname;
        installed.append(model);
    };
    QFile manifest(root() + "/installed.json");
    if (manifest.open(QIODevice::ReadOnly))
        for (const auto &value : QJsonDocument::fromJson(manifest.readAll()).array()) add(value.toObject().toVariantMap());
    if (m_installed != installed) { m_installed = installed; emit installedChanged(); }
}

QVariantList AssistantModels::supportModels() const
{
    QFile source(":/qt/qml/Pilon/assistant-models.json");
    if (!source.open(QIODevice::ReadOnly)) return {};
    const auto files = QJsonDocument::fromJson(source.readAll()).object().value("files").toArray();
#ifdef Q_OS_ANDROID
    const QString platform = "android";
#else
    const QString platform = "desktop";
#endif
    QVariantList models;
    for (const auto &pair : {qMakePair(QString("ppocrv5"), tr("Handwriting recognition")),
                            qMakePair(QString("whisper-large-v3-turbo"), tr("Speech recognition")),
                            qMakePair(QString("breeze-tts-2"), tr("Speech output"))}) {
        QVariantList selected; qint64 bytes = 0; bool installed = true;
        for (const auto &value : files) {
            const auto file = value.toObject().toVariantMap();
            const auto path = file.value("path").toString();
            if (!path.startsWith(pair.first + '/')) continue;
            if (file.contains("platform") && file.value("platform").toString() != platform) continue;
            selected.append(file); bytes += file.value("bytes").toLongLong();
            const QFileInfo info(root() + '/' + path);
            installed = installed && info.isFile() && info.size() == file.value("bytes").toLongLong();
        }
        models.append(QVariantMap{{"id", pair.first}, {"name", pair.second}, {"bytes", bytes},
            {"installed", installed && !selected.isEmpty()}, {"files", selected}});
    }
    return models;
}

void AssistantModels::downloadSupport(const QString &id)
{
    if (m_downloading || m_inspecting) return;
    for (const auto &value : supportModels()) {
        const auto model = value.toMap();
        if (model.value("id") != id) continue;
        m_downloadFiles.clear(); QVariantList targets;
        for (const auto &item : model.value("files").toList()) {
            auto file = item.toMap();
            file.insert("size", file.value("bytes"));
            if (!file.contains("url")) {
                QUrl url("https://huggingface.co");
                url.setPath('/' + file.value("repository").toString() + "/resolve/"
                    + file.value("revision").toString() + '/' + file.value("filename").toString());
                file.insert("url", url.toString());
            }
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

void AssistantModels::inspect(const QString &link)
{
    if (m_downloading) return;
    if (m_inspectReply) { m_inspectReply->disconnect(this); m_inspectReply->abort(); m_inspectReply->deleteLater(); m_inspectReply = nullptr; }
    m_repository.clear(); m_error.clear(); m_status.clear(); m_inspecting = false;
    emit repositoryChanged();
    const auto id = repositoryId(link);
    if (id.isEmpty()) {
        if (!link.trimmed().isEmpty()) m_error = tr("Paste a Hugging Face model repository URL or owner/repository.");
        emit stateChanged(); return;
    }
    m_inspecting = true; m_status = tr("Reading model information…"); emit stateChanged();
    QUrl url = m_hub;
    url.setPath("/api/models/" + id); url.setQuery("blobs=true");
    m_inspectReply = m_network.get(requestFor(url));
    connect(m_inspectReply, &QNetworkReply::readyRead, this, [this] {
        if (m_inspectReply && m_inspectReply->bytesAvailable() > 16 * 1024 * 1024) m_inspectReply->abort();
    });
    connect(m_inspectReply, &QNetworkReply::finished, this, [this] {
        auto reply = m_inspectReply; m_inspectReply = nullptr; m_inspecting = false; m_status.clear();
        if (reply->error() != QNetworkReply::NoError) {
            const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            m_error = code == 401 || code == 403 ? tr("Access denied. For a private or gated model, accept its terms on Hugging Face and launch Pilon with HF_TOKEN set.")
                : tr("Unable to read the repository: %1").arg(reply->errorString());
        } else {
            const auto metadata = QJsonDocument::fromJson(reply->readAll()).object();
            if (repositoryId(metadata.value("id").toString()).isEmpty() || metadata.value("sha").toString().isEmpty())
                m_error = tr("The repository returned incomplete model information.");
            else {
                m_repository = describeRepository(metadata);
                if (m_repository.value("variants").toList().isEmpty()) m_error = tr("This repository has no complete GGUF or LiteRT-LM (.litertlm) model files.");
            }
        }
        reply->deleteLater(); emit repositoryChanged(); emit stateChanged();
    });
}

QString AssistantModels::memoryWarning(qint64 bytes, qint64 available, qint64 total)
{
    if (bytes <= 0 || total <= 0) return {};
    // A lower bound: weights plus runtime overhead. Context/KV cache and other
    // applications can require more; GPU fitting may spill into system RAM.
    const qint64 required = bytes + std::max(GiB * 2, bytes / 5);
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

QVariantList AssistantModels::selectedFiles(int variant, int projector) const
{
    const auto variants = m_repository.value("variants").toList();
    if (variant < 0 || variant >= variants.size()) return {};
    auto files = variants[variant].toMap().value("files").toList();
    const auto projectors = m_repository.value("projectors").toList();
    if (variants[variant].toMap().value("format") != "litertlm" && projector >= 0 && projector < projectors.size())
        files.append(projectors[projector]);
    return files;
}

QString AssistantModels::downloadWarning(int variant, int projector) const
{
    qint64 bytes = 0;
    for (const auto &file : selectedFiles(variant, projector)) bytes += file.toMap().value("size").toLongLong();
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
    if (m_inspecting || m_downloading) return;
    if (projector < -1 || projector >= m_repository.value("projectors").toList().size()) {
        failDownload(tr("Choose a valid multimodal projector.")); return;
    }
    m_downloadFiles = selectedFiles(variant, projector);
    if (m_downloadFiles.isEmpty()) return;
    m_total = 0; m_completed = 0; m_received = 0; m_fileIndex = 0; m_error.clear();
    for (const auto &file : m_downloadFiles) m_total += file.toMap().value("size").toLongLong();
    if (!QDir().mkpath(root())) { failDownload(tr("Unable to create the models directory.")); return; }
    QStorageInfo disk(root());
    if (disk.isValid() && disk.bytesAvailable() < m_total) { failDownload(tr("Not enough free disk space for this model.")); return; }
    m_pendingModel = m_repository;
    m_pendingModel.remove("variants"); m_pendingModel.remove("projectors");
    const auto chosen = m_repository.value("variants").toList()[variant].toMap();
    const bool liteRt = chosen.value("format") == "litertlm";
    if (liteRt) projector = -1;
    m_pendingModel["format"] = chosen.value("format", "gguf");
    const auto id = m_repository.value("id").toString();
    const auto revision = m_repository.value("revision").toString();
    static const QRegularExpression commit("^[a-fA-F0-9]{40,64}$");
    if (repositoryId(id).isEmpty() || !commit.match(revision).hasMatch()) { failDownload(tr("Invalid repository revision.")); return; }
    const QString directory = "hf/" + id + '/' + revision + '/';
    m_pendingModel["id"] = id + '/' + chosen.value("filename").toString();
    m_pendingModel["name"] = QFileInfo(chosen.value("filename").toString()).completeBaseName();
    if (!nickname.trimmed().isEmpty()) m_pendingModel["nickname"] = nickname.trimmed();
    m_pendingModel["weights"] = directory + m_downloadFiles.first().toMap().value("path").toString();
    m_pendingModel["projector"] = projector >= 0 ? directory + m_downloadFiles.last().toMap().value("path").toString() : QString{};
    if (projector < 0 && !liteRt) {
        m_pendingModel["vision"] = false; m_pendingModel["audio"] = false; m_pendingModel["video"] = false;
        m_pendingModel["modalities"] = QStringList{tr("Text")};
    }
    QVariantList files;
    for (const auto &value : m_downloadFiles) {
        auto file = value.toMap(); file["path"] = directory + file.value("path").toString(); files.append(file);
    }
    m_pendingModel["files"] = files;
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
        if (existing.open(QIODevice::ReadOnly)) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(existing.readAll(), &error);
            if (error.error != QJsonParseError::NoError || !document.isArray()) { failDownload(tr("The installed-model registry could not be read.")); return; }
            for (const auto &value : document.array())
                if (value.toObject().value("id").toString() != m_pendingModel.value("id").toString()) models.append(value);
        }
        models.append(QJsonObject::fromVariantMap(m_pendingModel));
        QSaveFile manifest(root() + "/installed.json"); const auto data = QJsonDocument(models).toJson();
        if (!manifest.open(QIODevice::WriteOnly) || manifest.write(data) != data.size() || !manifest.commit()) {
            failDownload(tr("Unable to save the installed-model registry: %1").arg(manifest.errorString())); return;
        }
        m_downloading = false; m_status = tr("Model downloaded."); refresh(); emit stateChanged();
        emit modelDownloaded(m_pendingModel.value("id").toString()); emit downloadCompleted(); return;
    }
    const auto file = m_downloadFiles[m_fileIndex].toMap();
    const auto target = m_pendingModel.value("files").toList()[m_fileIndex].toMap().value("path").toString();
    if (!safePath(target) || !QDir().mkpath(QFileInfo(root() + '/' + target).absolutePath())) {
        failDownload(tr("Unable to create the model folder.")); return;
    }
    m_file = std::make_unique<QSaveFile>(root() + '/' + target);
    if (!m_file->open(QIODevice::WriteOnly)) { failDownload(m_file->errorString()); return; }
    m_received = 0; m_hash.reset(); m_magic.clear();
    QUrl url = m_hub;
    url.setPath('/' + m_repository.value("id").toString() + "/resolve/" + m_repository.value("revision").toString() + '/' + file.value("path").toString());
    if (file.contains("url")) url = QUrl(file.value("url").toString());
    m_downloadReply = m_network.get(requestFor(url));
    m_downloadReply->setReadBufferSize(1024 * 1024);
    m_status = tr("Downloading %1 (%2/%3)…").arg(file.value("path").toString()).arg(m_fileIndex + 1).arg(m_downloadFiles.size());
    connect(m_downloadReply, &QNetworkReply::readyRead, this, &AssistantModels::readDownload);
    connect(m_downloadReply, &QNetworkReply::finished, this, [this, file] {
        readDownload();
        if (!m_downloadReply) return;
        auto reply = m_downloadReply; m_downloadReply = nullptr;
        const auto error = reply->error(); const auto message = reply->errorString(); reply->deleteLater();
        if (error != QNetworkReply::NoError) { failDownload(tr("Download failed: %1").arg(message)); return; }
        const auto expectedHash = file.value("sha256").toString().toLatin1();
        const bool validMagic = file.value("format") == "support"
            || (file.value("format") == "litertlm" ? m_magic == "LITERTLM" : m_magic.startsWith("GGUF"));
        if (!validMagic || m_received != file.value("size").toLongLong() || (!expectedHash.isEmpty() && m_hash.result().toHex() != expectedHash)) {
            failDownload(tr("The model download failed its integrity check. Please try again.")); return;
        }
        if (!m_file->commit()) { failDownload(m_file->errorString()); return; }
        m_file.reset(); m_completed += m_received; m_received = 0; ++m_fileIndex;
        emit stateChanged(); downloadNext();
    });
    emit stateChanged();
}

void AssistantModels::readDownload()
{
    if (!m_downloadReply || !m_file) return;
    while (m_downloadReply->bytesAvailable()) {
        const auto bytes = m_downloadReply->read(256 * 1024);
        if (bytes.isEmpty()) break;
        if (m_magic.size() < 8) m_magic += bytes.first(qMin(qsizetype(8 - m_magic.size()), bytes.size()));
        if (m_file->write(bytes) != bytes.size()) { failDownload(tr("Unable to write the model: %1").arg(m_file->errorString())); return; }
        m_hash.addData(bytes); m_received += bytes.size();
        if (m_received > m_downloadFiles[m_fileIndex].toMap().value("size").toLongLong()) {
            failDownload(tr("The download is larger than the repository's declared file size.")); return;
        }
    }
    emit stateChanged();
}

void AssistantModels::cancelDownload()
{
    if (m_downloadReply) { m_downloadReply->disconnect(this); m_downloadReply->abort(); m_downloadReply->deleteLater(); m_downloadReply = nullptr; }
    if (m_file) { m_file->cancelWriting(); m_file.reset(); }
    if (m_downloading) { m_downloading = false; m_status = tr("Download cancelled."); emit stateChanged(); }
}

void AssistantModels::failDownload(const QString &message)
{
    cancelDownload(); m_status.clear(); m_error = message; emit stateChanged();
}
