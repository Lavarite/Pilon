#include "../../../src/core/workspace/pilonworkspace.h"
#include "assistantmanager.h"

#include <QDir>
#include <QDateTime>
#include <QCryptographicHash>
#include <QDrag>
#include <QFileInfo>
#include <QFontInfo>
#include <QIcon>
#include <QMimeDatabase>
#include <QMimeData>
#include <QPdfWriter>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextDocument>
#include <QUuid>
#include <algorithm>
#include <csignal>
#include <unistd.h>

namespace {
class ArtifactDocument : public QTextDocument
{
    // Report text cannot fetch URLs or read local files through embedded images.
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

bool validPath(const QString &path)
{
    return QDir::isAbsolutePath(path) && !path.contains(QChar::Null)
        && !path.contains('\n') && !path.contains('\r');
}

QString writeArtifact(const QString &path, const QByteArray &content)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return file.errorString();
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (QFileInfo(path).suffix().compare("pdf", Qt::CaseInsensitive) == 0) {
        {
            QPdfWriter writer(&file);
            writer.setCreator("Pilon");
            writer.setTitle(QFileInfo(path).completeBaseName());
            writer.setPageSize(QPageSize(QPageSize::A4));
            writer.setPageMargins(QMarginsF(15, 15, 15, 15));
            ArtifactDocument document;
            auto font = document.defaultFont();
            if (font.pixelSize() > 0) {
                font.setPointSizeF(QFontInfo(font).pointSizeF());
                document.setDefaultFont(font);
            }
            document.setMarkdown(QString::fromUtf8(content));
            document.print(&writer);
        } // Finish the PDF before atomically committing its file.
        if (file.error() != QFileDevice::NoError || file.pos() == 0)
            return QObject::tr("Unable to render the PDF: %1").arg(file.errorString());
    } else if (file.write(content) != content.size()) {
        return file.errorString();
    }
    if (!file.commit()) return file.errorString();
    return {};
}
}

void AssistantManager::initializeFileTools()
{
    m_localTool.setChildProcessModifier([] { ::setsid(); });
    m_localTool.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_localTool, &QProcess::readyReadStandardOutput, this, [this] {
        const auto output = m_localTool.readAllStandardOutput();
        m_localToolOutput += output;
    });
    connect(&m_localTool, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!m_toolExecuting || !m_busy || m_currentTool.isEmpty()) return;
        finishTool({{"exit_code", status == QProcess::NormalExit ? code : -1},
            {"output", QString::fromUtf8(m_localToolOutput)},
            {"truncated", false}});
    });
    connect(&m_localTool, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !m_toolExecuting || !m_busy) return;
        finishTool({{"error", m_localTool.errorString()}});
    });
}

void AssistantManager::stopLocalTool()
{
    if (m_localTool.state() == QProcess::NotRunning) return;
    const auto pid = m_localTool.processId();
    if (pid > 0) ::kill(-pid, SIGKILL);
    m_localTool.kill();
}

QString AssistantManager::artifactRoot() const
{
    if (!qEnvironmentVariable("PILON_ARTIFACT_DIR").isEmpty())
        return qEnvironmentVariable("PILON_ARTIFACT_DIR");
    if (qEnvironmentVariableIsSet("PILON_EPHEMERAL_CHAT"))
        return m_sessionDirectory.path() + "/artifacts";
    return qEnvironmentVariableIsSet("PILON_AGENT_DIR") ? workspaceRoot() + "/artifacts"
        : PilonWorkspace::artifactsDirectory();
}

QJsonObject AssistantManager::recordArtifact(const QString &path, bool owned, const QString &existingId)
{
    const QFileInfo info(path);
    if (!validPath(path) || !info.isFile() || !info.isReadable())
        return {{"error", "Artifact must be an existing readable local file."}};
    const QString id = existingId.isEmpty() ? QUuid::createUuid().toString(QUuid::Id128) : existingId;
    const QJsonObject artifact{{"id", id}, {"path", info.absoluteFilePath()}, {"name", info.fileName()},
        {"target", "local"}, {"mime", QMimeDatabase().mimeTypeForFile(info).name()}, {"bytes", info.size()}};
    const QVariantMap message{{"role", "artifact"}, {"id", id}, {"artifact", artifact.toVariantMap()},
        {"owned", owned}};
    bool replaced = false;
    for (auto &value : m_messages) {
        const auto old = value.toMap();
        if (old.value("role") == "artifact" && old.value("id").toString() == id) {
            value = message;
            replaced = true;
            break;
        }
    }
    if (!replaced) m_messages.append(message);
    emit messagesChanged();
    return artifact;
}

void AssistantManager::openArtifact(const QString &id)
{
    for (const auto &value : messages()) {
        const auto message = value.toMap();
        if (message.value("role") != "artifact" || message.value("id").toString() != id) continue;
        const auto path = PilonWorkspace::currentPublicPath(PilonWorkspace::configuredRoot(),
            message.value("artifact").toMap().value("path").toString());
        if (!validPath(path) || !QFileInfo(path).isFile()) {
            m_error = tr("The artifact has moved or is no longer available: %1").arg(path);
            emit stateChanged();
            return;
        }
        const QFileInfo info(path);
        emit artifactLocationRequested(QFileInfo(info.absolutePath()).canonicalFilePath() + '/' + info.fileName());
        return;
    }
}

void AssistantManager::startArtifactDrag(const QString &id)
{
    for (const auto &value : messages()) {
        const auto message = value.toMap();
        if (message.value("role") != "artifact" || message.value("id").toString() != id) continue;
        const auto path = PilonWorkspace::currentPublicPath(PilonWorkspace::configuredRoot(),
            message.value("artifact").toMap().value("path").toString());
        const QFileInfo file(path);
        if (!validPath(path) || !file.isFile() || !file.isReadable()) {
            m_error = tr("The artifact has moved or is no longer available: %1").arg(path);
            emit stateChanged();
            return;
        }
        auto *mimeData = new QMimeData;
        mimeData->setUrls({QUrl::fromLocalFile(file.absoluteFilePath())});
        auto *drag = new QDrag(this);
        drag->setMimeData(mimeData);
        drag->setPixmap(QIcon(":/qt/qml/Pilon/icons/document.svg").pixmap(36, 36));
        drag->exec(Qt::CopyAction, Qt::CopyAction);
        drag->deleteLater();
        return;
    }
}

QJsonObject AssistantManager::readLocalFile(const QJsonObject &args)
{
    QString path = args.value("path").toString();
    if (!path.isEmpty() && !QDir::isAbsolutePath(path)) path = QDir(workingDirectory()).absoluteFilePath(path);
    if (!validPath(path)) return {{"error", "Use a local file path."}};
    const QFileInfo info(path);
    const QString key = info.canonicalFilePath() + ':' + QString::number(args.value("offset").toInteger()) + ':' + QString::number(args.value("max_bytes").toInteger());
    const QString revision = QString::number(info.size()) + ':' + QString::number(info.lastModified().toMSecsSinceEpoch()) + ':' + QString::number(info.metadataChangeTime().toMSecsSinceEpoch());
    if (!args.value("refresh").toBool() && m_readFiles.value(key).first == revision)
        return {{"path", path}, {"already_read", true}, {"previous_call_id", m_readFiles.value(key).second},
            {"message", "This unchanged range is already in the current turn. Reuse that result; set refresh to true if the content is needed again."}};
    const auto read = [&]() -> QJsonObject {
        QFile file(path);
        if (!info.isFile() || !file.open(QIODevice::ReadOnly)) {
            return QJsonObject({{"error", "File is unavailable or unreadable."}});
        }
        const qint64 offset = args.value("offset").toInteger(0);
        const qint64 limit = args.value("max_bytes").toInteger(0);
        if (offset < 0 || offset > file.size() || !file.seek(offset)) {
            return QJsonObject({{"error", "offset must be a byte position within the file."}});
        }
        if (info.suffix().compare("pin", Qt::CaseInsensitive) == 0 && offset == 0 && limit == 0) {
            auto note = QJsonDocument::fromJson(file.readAll()).object();
            if (note.value("format") != "pilon-note" || note.value("formatVersion").toInt() != 1) {
                return QJsonObject({{"error", "Unsupported Pilon note format."}});
            }
            const auto directory = workingDirectory() + "/note-previews/" + QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex());
            if (!QDir().mkpath(directory)) { return QJsonObject({{"error", "Unable to prepare note images."}}); }
            QJsonArray images;
            for (const auto &value : note.take("pages").toArray()) {
                const auto png = QByteArray::fromBase64(value.toObject().value("data").toString().toLatin1());
                const auto image = directory + '/' + QString::number(images.size() + 1) + ".png";
                QSaveFile out(image);
                if (!out.open(QIODevice::WriteOnly) || out.write(png) != png.size() || !out.commit()) { return QJsonObject({{"error", "Unable to prepare a note image."}}); }
                images.append(image);
            }
            note.remove("assets");
            return QJsonObject({{"path", path}, {"content", note}, {"images", images},
                {"next_step", "Read the text, elements and handwriting lines here. Use view_image on the image paths to inspect the rendered pages. The original .pin also embeds its assets."}});
        }
        const auto bytes = limit > 0 ? file.read(limit) : file.readAll();
        if (bytes.contains('\0')) {
            return QJsonObject({{"error", "This is a binary file. Use an appropriate command to inspect it."}});
        }
        return QJsonObject({{"path", path}, {"content", QString::fromUtf8(bytes)},
            {"offset", offset}, {"next_offset", file.pos()}, {"truncated", !file.atEnd()}});
    };
    const auto result = read();
    if (!result.contains("error")) m_readFiles.insert(key, {revision, m_currentTool.value("id").toString()});
    return result;
}

bool AssistantManager::runFileTool(const QString &name, const QJsonObject &args)
{
    if (name == "save_artifact_project") {
        const QString path = QFileInfo(args.value("path").toString()).canonicalFilePath();
        const QString root = QFileInfo(artifactRoot() + '/' + m_chatId).canonicalFilePath();
        if (root.isEmpty() || !path.startsWith(root + '/') || !QFileInfo(path).isDir()) {
            finishTool({{"error", "Choose a directory inside this chat's artifact workspace."}}); return true;
        }
        const auto id = addProject(path);
        finishTool(id.isEmpty() ? QJsonObject{{"error", "Unable to save the project."}}
                              : QJsonObject::fromVariantMap(project(id)));
        return true;
    }
    if (name == "local_exec") {
        const QString command = args.value("command").toString();
        QString directory = args.value("working_directory").toString();
        if (args.contains("working_directory") && !args.value("working_directory").isString()) {
            finishTool({{"error", "working_directory must be a string."}});
            return true;
        }
        if (directory.isEmpty()) directory = workingDirectory();
        if (!QDir::isAbsolutePath(directory)) directory = QDir(workingDirectory()).absoluteFilePath(directory);
        if (command.trimmed().isEmpty() || command.contains(QChar::Null)
            || !validPath(directory) || !QFileInfo(directory).isDir()) {
            finishTool({{"error", "Provide a nonempty command and an existing absolute working_directory."}});
            return true;
        }
        m_toolDirectory = QFileInfo(directory).canonicalFilePath();
        m_toolCommand = command;
        requestToolApproval(tr("This device: %1\n%2").arg(m_toolDirectory, m_toolCommand));
        return true;
    }
    if (name == "read_files") {
        const auto files = args.value("files").toArray();
        if (files.isEmpty() || files.size() > 32) { finishTool({{"error", "Provide between 1 and 32 files."}}); return true; }
        QJsonArray results;
        for (const auto &file : files) results.append(file.isObject() ? readLocalFile(file.toObject()) : QJsonObject{{"error", "Each file must have a path."}});
        finishTool({{"files", results}}); return true;
    }
    if (name == "list_directory" || name == "read_file") {
        QString path = args.value("path").toString();
        if (path.isEmpty() && name == "list_directory")
            path = workingDirectory();
        if (!path.isEmpty() && !QDir::isAbsolutePath(path)) path = QDir(workingDirectory()).absoluteFilePath(path);
        if (!validPath(path)) {
            finishTool({{"error", "Use an absolute local path."}});
            return true;
        }
        const QFileInfo info(path);
        if (name == "list_directory") {
            if (!info.isDir() || !info.isReadable()) {
                finishTool({{"error", "Directory is unavailable or unreadable."}});
                return true;
            }
            const auto entries = QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot,
                                                          QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
            const qint64 limit = args.value("limit").toInteger(0);
            QJsonArray result;
            for (const auto &entry : entries) {
                if (limit > 0 && result.size() >= limit) break;
                result.append(QJsonObject{{"name", entry.fileName()}, {"path", entry.absoluteFilePath()},
                    {"directory", entry.isDir()}, {"symlink", entry.isSymLink()}, {"bytes", entry.size()}});
            }
            finishTool({{"path", info.absoluteFilePath()}, {"entries", result},
                {"current_files_location", appContext().value("current_location")},
                {"truncated", limit > 0 && entries.size() > limit}});
        } else finishTool(readLocalFile(args));

        return true;
    }
    if (name == "publish_artifact") {
        finishTool(recordArtifact(args.value("path").toString(), false));
        return true;
    }
    if (name != "create_artifact" && name != "update_artifact") return false;
    const auto content = args.value("content").toString().toUtf8();
    if (!args.value("content").isString()) {
        finishTool({{"error", "content must be UTF-8 text. Publish an existing file for binary outputs."}});
        return true;
    }
    QString path;
    QString id;
    QString createdDirectory;
    if (name == "create_artifact") {
        const QString filename = args.value("filename").toString();
        if (filename.isEmpty() || filename == "." || filename == ".."
            || filename.contains('/') || filename.contains('\\')
            || std::any_of(filename.cbegin(), filename.cend(), [](QChar c) { return c.category() == QChar::Other_Control; })) {
            finishTool({{"error", "Provide a filename without directories or control characters."}});
            return true;
        }
        const QString chatDirectory = artifactRoot() + '/' + m_chatId;
        if (QFileInfo(artifactRoot()).isSymLink() || QFileInfo(chatDirectory).isSymLink() || !QDir().mkpath(chatDirectory)) {
            finishTool({{"error", "Unable to create the artifact directory."}});
            return true;
        }
        id = QUuid::createUuid().toString(QUuid::Id128);
        const QString directory = chatDirectory + '/' + id;
        if (!QDir().mkdir(directory)) {
            finishTool({{"error", "Unable to create a unique artifact directory."}});
            return true;
        }
        createdDirectory = directory;
        QFile::setPermissions(chatDirectory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        path = directory + '/' + filename;
    } else {
        id = args.value("id").toString();
        for (const auto &value : m_messages) {
            const auto message = value.toMap();
            if (message.value("role") == "artifact" && message.value("id").toString() == id)
                path = message.value("artifact").toMap().value("path").toString();
        }
        path = PilonWorkspace::currentPublicPath(PilonWorkspace::configuredRoot(), path);
        const auto info = QFileInfo(path);
        if (path.isEmpty() || !validPath(path) || !info.isFile()) {
            finishTool({{"error", "The artifact file is no longer available."}});
            return true;
        }
    }
    const QString error = writeArtifact(path, content);
    if (!error.isEmpty() && !createdDirectory.isEmpty()) {
        QDir().rmdir(createdDirectory);
        QDir().rmdir(QFileInfo(createdDirectory).absolutePath());
        QDir().rmdir(artifactRoot());
    }
    finishTool(error.isEmpty() ? recordArtifact(path, true, id) : QJsonObject{{"error", error}});
    return true;
}
