#include "assistantmedia.h"
#include "../../../src/core/workspace/pilonworkspace.h"
#ifdef Q_OS_ANDROID
#include "../../../src/core/documents/documentpreview.h"
#endif

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QUrl>
#include <QUuid>
#include <QtEndian>
#include <cmath>

namespace {
QString run(const QString &program, const QStringList &arguments,
            const std::shared_ptr<std::atomic_bool> &cancel, QByteArray *output = nullptr)
{
    if (program.isEmpty())
        return QStringLiteral("A required media utility is missing.");
    if (cancel->load()) return QStringLiteral("Cancelled");
    QElapsedTimer elapsed;
    elapsed.start();
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(3000))
        return process.errorString();
    QByteArray errors;
    while (!process.waitForFinished(100)) {
        errors += process.readAllStandardError();
        errors = errors.right(8192);
        if (output)
            *output += process.readAllStandardOutput();
        else
            process.readAllStandardOutput();
        if (cancel->load() || elapsed.hasExpired(600000)) {
            process.kill();
            process.waitForFinished(3000);
            return cancel->load() ? QStringLiteral("Cancelled") : QStringLiteral("Media processing timed out.");
        }
        if (process.state() == QProcess::NotRunning) break;
    }
    errors += process.readAllStandardError();
    if (output)
        *output += process.readAllStandardOutput();
    if (process.exitCode() == 0 && process.exitStatus() == QProcess::NormalExit) return {};
    const auto diagnostic = QString::fromUtf8(errors.right(8192)).trimmed();
    return diagnostic.isEmpty() ? QStringLiteral("Media utility exited without a diagnostic.") : diagnostic;
}

QJsonObject textPart(const QString &text)
{
    return {{"type", "text"}, {"text", text}};
}

constexpr qint64 maximumTranscriptBytes = 1024 * 1024;

QString assistantTranscriptPath(const QString &directory, const QString &attachmentId)
{
    static const QRegularExpression validId("^[a-f0-9]{32}$");
    if (!validId.match(attachmentId).hasMatch()) return {};
    const QFileInfo folder(directory);
    if (!folder.isDir() || folder.isSymLink() || QFileInfo(folder.absolutePath()).isSymLink()) return {};
    return QDir(directory).filePath(attachmentId + ".transcript.txt");
}
}

void saveAssistantAudioTranscript(const QString &directory, const QString &attachmentId, const QString &text,
                                  const std::shared_ptr<std::atomic_bool> &cancel)
{
    const auto path = assistantTranscriptPath(directory, attachmentId);
    if (path.isEmpty() || QFileInfo(path).isSymLink() || cancel->load()) return;
    QSaveFile transcript(path);
    const auto bytes = text.toUtf8();
    if (transcript.open(QIODevice::WriteOnly) && transcript.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        && transcript.write(bytes) == bytes.size() && !cancel->load())
        transcript.commit();
}

bool loadAssistantAttachmentTranscripts(QVariantList &attachments, const QString &directory)
{
    bool changed = false;
    for (auto &entry : attachments) {
        auto attachment = entry.toMap();
        if (!attachment.value("mime").toString().startsWith("audio/") || attachment.contains("transcript")) continue;
        const auto path = assistantTranscriptPath(directory, attachment.value("id").toString());
        if (path.isEmpty() || !QFileInfo(path).isFile() || QFileInfo(path).isSymLink()) continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        const auto bytes = file.read(maximumTranscriptBytes + 1);
        if (bytes.isEmpty() || bytes.size() > maximumTranscriptBytes) continue;
        attachment.insert("transcript", QString::fromUtf8(bytes));
        entry = attachment;
        changed = true;
    }
    return changed;
}

void loadAssistantAudioTranscripts(QVariantList &messages, const QString &directory)
{
    for (auto &value : messages) {
        auto message = value.toMap();
        auto attachments = message.value("attachments").toList();
        if (loadAssistantAttachmentTranscripts(attachments, directory)) {
            message.insert("attachments", attachments);
            value = message;
        }
    }
}

bool applyAssistantAttachmentTranscripts(QVariantList &attachments, const QVariantMap &transcripts)
{
    if (transcripts.isEmpty()) return false;
    bool changed = false;
    for (auto &entry : attachments) {
        auto attachment = entry.toMap();
        const auto it = transcripts.constFind(attachment.value("id").toString());
        if (it == transcripts.cend() || it.value() == attachment.value("transcript")) continue;
        attachment.insert("transcript", it.value());
        entry = attachment;
        changed = true;
    }
    return changed;
}

bool applyAssistantAudioTranscripts(QVariantList &messages, const QVariantMap &transcripts, const QString &messageId)
{
    if (transcripts.isEmpty()) return false;
    bool changed = false;
    for (auto &value : messages) {
        auto message = value.toMap();
        if (!messageId.isEmpty() && message.value("messageId").toString() != messageId) continue;
        auto attachments = message.value("attachments").toList();
        if (!applyAssistantAttachmentTranscripts(attachments, transcripts)) continue;
        message.insert("attachments", attachments);
        value = message;
        changed = true;
        if (!messageId.isEmpty()) break;
    }
    return changed;
}

QVariantMap transcribeAssistantAudio(const QString &wavPath, const QString &whisperExecutable,
                                    const QString &whisperModel, const std::shared_ptr<std::atomic_bool> &cancel)
{
    if (!QFileInfo::exists(whisperModel))
        return {{"error", QStringLiteral("Download a speech recognition model in Settings → Audio models.")}};
    const auto stem = wavPath + ".transcript";
    const auto output = stem + ".txt";
    const auto cleanup = qScopeGuard([&] { QFile::remove(output); });
    const auto error = run(whisperExecutable, {"-m", whisperModel, "-f", wavPath, "-l", "auto",
                            "-t", "8", "-nt", "-otxt", "-of", stem}, cancel);
    if (!error.isEmpty()) return {{"error", error}};
    QFile transcript(output);
    if (!transcript.open(QIODevice::ReadOnly)) return {{"error", transcript.errorString()}};
    const auto bytes = transcript.read(maximumTranscriptBytes + 1);
    if (bytes.size() > maximumTranscriptBytes)
        return {{"error", QStringLiteral("The transcript exceeds the supported size.")}};
    const auto speech = QString::fromUtf8(bytes).trimmed();
    if (speech.isEmpty() || speech == "[BLANK_AUDIO]")
        return {{"error", QStringLiteral("No intelligible speech was transcribed. Please try a clearer recording.")}};
    return {{"text", speech}};
}

QVariantMap assistantAudioSummary(const QString &wavPath)
{
    QFile file(wavPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const auto header = file.read(12);
    if (header.size() != 12 || header.first(4) != "RIFF" || header.sliced(8, 4) != "WAVE") return {};
    const qint64 containerEnd = qint64(qFromLittleEndian<quint32>(header.constData() + 4)) + 8;
    if (containerEnd < file.pos() || containerEnd > file.size()) return {};
    quint32 sampleRate = 0;
    quint16 channels = 0, blockAlign = 0;
    qint64 frames = 0;
    while (containerEnd - file.pos() >= 8) {
        const auto chunk = file.read(8);
        if (chunk.size() != 8) return {};
        const qint64 size = qFromLittleEndian<quint32>(chunk.constData() + 4);
        const qint64 next = file.pos() + size + size % 2;
        if (next > containerEnd) return {};
        if (chunk.startsWith("fmt ")) {
            if (size < 16 || sampleRate) return {};
            const auto format = file.read(16);
            if (format.size() != 16 || qFromLittleEndian<quint16>(format.constData()) != 1
                || qFromLittleEndian<quint16>(format.constData() + 14) != 16) return {};
            channels = qFromLittleEndian<quint16>(format.constData() + 2);
            sampleRate = qFromLittleEndian<quint32>(format.constData() + 4);
            blockAlign = qFromLittleEndian<quint16>(format.constData() + 12);
            if (!sampleRate || !channels || blockAlign != channels * 2) return {};
        } else if (chunk.startsWith("data")) {
            if (!blockAlign || !size || size % blockAlign) return {};
            frames = size / blockAlign;
            break;
        }
        if (!file.seek(next)) return {};
    }
    if (!frames) return {};
    QVariantList levels;
    double maximum = 0.01;
    for (int bin = 0; bin < 48; ++bin) {
        const qint64 count = (frames * (bin + 1) / 48 - frames * bin / 48) * channels;
        double squares = 0;
        for (qint64 remaining = count * 2; remaining; ) {
            const auto size = qMin<qint64>(remaining, 32 * 1024);
            const auto block = file.read(size);
            if (block.size() != size) return {};
            for (qsizetype offset = 0; offset < block.size(); offset += 2) {
                const double sample = qFromLittleEndian<qint16>(block.constData() + offset) / 32768.0;
                squares += sample * sample;
            }
            remaining -= block.size();
        }
        const double level = std::sqrt(squares / qMax<qint64>(1, count));
        maximum = qMax(maximum, level);
        levels.append(level);
    }
    for (QVariant &level : levels)
        level = level.toDouble() / maximum;
    return {{"waveform", levels}, {"duration", frames / double(sampleRate)}};
}

QVariantMap prepareAssistantMedia(const QVariantList &attachments, const QString &text,
                                 bool nativeAudio, const QString &directory,
                                 const QString &whisperExecutable, const QString &whisperModel,
                                 const std::shared_ptr<std::atomic_bool> &cancel, bool collectTranscriptions,
                                 const std::function<void(const QString &, const QString &)> &transcriptReady)
{
    QJsonArray content;
    QJsonArray audioParts;
    QString attachedText;
    bool hasAudio = false;
    QVariantList preparedAttachments;
    QVariantMap transcriptionInputs;
    bool prepared = false;
    const auto clearUnusedTranscriptions = qScopeGuard([&] {
        if (!prepared) for (const auto &input : std::as_const(transcriptionInputs)) QFile::remove(input.toString());
    });
    for (const QVariant &value : attachments) {
        if (cancel->load())
            return {{"error", "Cancelled"}};
        QVariantMap attachment = value.toMap();
        const QString path = attachment.value("path").toString();
        const QString name = attachment.value("name").toString();
        if (!QFileInfo::exists(path)
            && !QFile::copy(attachment.value("sourcePath").toString(), path))
            return {{"error", name + QStringLiteral(": Unable to stage the attachment.")}};
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        attachment.insert("url", QUrl::fromLocalFile(path));
        const QString mime = QMimeDatabase().mimeTypeForFile(path).name();
        // Native recordings are already inline content, not file references
        // for the agent to open. Keep their names and IDs in the attachment UI.
        if (!(nativeAudio && mime.startsWith("audio/")))
            attachedText += QStringLiteral("\nAttachment %1: %2\n")
                                .arg(attachment.value("id").toString(), name);
        auto failure = [&](const QString &message) -> QVariantMap {
            return {{"error", name + QStringLiteral(": ") + message}};
        };
        if (mime.startsWith("image/")) {
            QImageReader reader(path);
            reader.setAutoTransform(true);
            const QSize size = reader.size();
            if (size.width() > 4096 || size.height() > 4096)
                reader.setScaledSize(size.scaled(4096, 4096, Qt::KeepAspectRatio));
            const QImage image = reader.read();
            if (image.isNull())
                return failure(reader.errorString());
            QByteArray png;
            QBuffer buffer(&png);
            if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
                return failure(QStringLiteral("Unable to encode the attachment image."));
            content.append(QJsonObject{{"type", "image_url"},
                {"image_url", QJsonObject{{"url", "data:image/png;base64," + QString::fromLatin1(png.toBase64())}}}});
        } else if (mime.startsWith("audio/")) {
            const auto cachedSpeech = attachment.value("transcript").toString();
            hasAudio = true;
            if (!nativeAudio && !cachedSpeech.isEmpty()) {
                attachedText += QStringLiteral("Speaker's message (transcribed locally):\n") + cachedSpeech + '\n';
                preparedAttachments.append(attachment);
                continue;
            }
#ifdef Q_OS_ANDROID
            const QString ffmpeg = PilonWorkspace::packagedExecutable(QStringLiteral("ffmpeg"));
#else
            const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
#endif
            const QString stem = QDir(directory).filePath(QUuid::createUuid().toString(QUuid::Id128));
            const QString wav = stem + ".wav";
            auto cleanupWav = qScopeGuard([&] { QFile::remove(wav); });
            QString error = run(ffmpeg, {"-nostdin", "-v", "error", "-i", path, "-vn", "-ac", "1",
                                         "-ar", "16000", "-c:a", "pcm_s16le", wav}, cancel);
            if (!error.isEmpty())
                return failure(error);
            const QVariantMap summary = assistantAudioSummary(wav);
            if (summary.isEmpty())
                return failure(QStringLiteral("The media utility produced no valid audio frames."));
            for (auto it = summary.cbegin(); it != summary.cend(); ++it)
                attachment.insert(it.key(), it.value());
            if (nativeAudio) {
                QStringList chunks{QFileInfo(wav).fileName()};
                // Avoid a second FFmpeg process for microphone-sized recordings.
                if (summary.value("duration").toDouble() > 30.0) {
                    // One-second frames keep cuts within Gemma's 30-second window.
                    // Stream-copy segmentation can round past it and lose speech.
                    const QString pattern = stem + "-%03d.wav";
                    error = run(ffmpeg, {"-nostdin", "-v", "error", "-i", wav,
                        "-af", "asetnsamples=n=16000:p=0", "-f", "segment",
                        "-segment_time", "30", "-c:a", "pcm_s16le", pattern}, cancel);
                    if (!error.isEmpty())
                        return failure(error);
                    chunks = QDir(directory).entryList(
                        {QFileInfo(stem).fileName() + "-*.wav"}, QDir::Files, QDir::Name);
                }
                if (chunks.isEmpty())
                    return failure(QStringLiteral("The recording contains no audio frames."));
                const auto cleanupChunks = qScopeGuard([&] {
                    for (const auto &chunk : chunks)
                        if (chunk != QFileInfo(wav).fileName()) QFile::remove(QDir(directory).filePath(chunk));
                });
                for (const QString &chunk : chunks) {
                    QFile file(QDir(directory).filePath(chunk));
                    if (!file.open(QIODevice::ReadOnly))
                        return failure(file.errorString());
                    audioParts.append(QJsonObject{{"type", "input_audio"}, {"input_audio",
                        QJsonObject{{"data", QString::fromLatin1(file.readAll().toBase64())}, {"format", "wav"}}}});
                }
                const auto id = attachment.value("id").toString();
                if (collectTranscriptions && !whisperExecutable.isEmpty() && !whisperModel.isEmpty()
                    && !id.isEmpty() && cachedSpeech.isEmpty()) {
                    transcriptionInputs.insert(id, wav);
                    cleanupWav.dismiss();
                }
            }
            if (!nativeAudio) {
                const auto transcript = transcribeAssistantAudio(wav, whisperExecutable, whisperModel, cancel);
                if (transcript.contains("error")) return failure(transcript.value("error").toString());
                const auto speech = transcript.value("text").toString();
                attachment.insert("transcript", speech);
                if (transcriptReady) transcriptReady(attachment.value("id").toString(), speech);
                attachedText += QStringLiteral("Speaker's message (transcribed locally):\n") + speech + '\n';
            }
        } else if (mime.startsWith("video/")) {
            // llama.cpp decodes video locally. Limit duration before model-specific processing.
#ifdef Q_OS_ANDROID
            const QString ffprobe = PilonWorkspace::packagedExecutable(QStringLiteral("ffprobe"));
#else
            const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
#endif
            QByteArray probe;
            const QString error = run(ffprobe,
                {"-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", path}, cancel, &probe);
            bool ok = false;
            const double seconds = probe.trimmed().toDouble(&ok);
            if (!error.isEmpty() || !ok || !std::isfinite(seconds) || seconds <= 0)
                return failure(QStringLiteral("Unable to read the video duration."));
            if (seconds > (nativeAudio ? 60.0 : 300.0))
                return failure(nativeAudio ? QStringLiteral("Gemma video input is limited to 60 seconds.")
                                           : QStringLiteral("Use a clip up to 5 minutes for this local context size."));
            content.append(QJsonObject{{"type", "input_video"}, {"input_video",
                QJsonObject{{"url", QUrl::fromLocalFile(path).toString()}}}});
        } else {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return failure(file.errorString());
            if (mime.startsWith("text/") || mime.contains("json") || mime.contains("xml")
                || mime.contains("javascript") || mime.contains("yaml")) {
                if (file.size() > 256 * 1024)
                    return failure(QStringLiteral("Text attachments must be at most 256 KiB."));
                attachedText += QString::fromUtf8(file.readAll()) + '\n';
            } else if (mime == "application/pdf") {
                const QString stem = QDir(directory).filePath(QUuid::createUuid().toString(QUuid::Id128));
#ifdef Q_OS_ANDROID
                const auto opened = DocumentPreview::decodeDocument({{"source", path}, {"command", "open"}}, cancel.get());
                if (opened.contains("error")) return failure(opened.value("error").toString());
                QStringList rendered;
                for (int page = 0; page < qMin<qsizetype>(20, opened.value("pages").toList().size()); ++page) {
                    const QString output = stem + QString("-%1.png").arg(page + 1, 2, 10, QChar('0'));
                    const auto result = DocumentPreview::decodeDocument({{"source", path}, {"command", "render"},
                        {"page", page}, {"resolution", 1600}, {"output", output}}, cancel.get());
                    if (result.contains("error")) {
                        for (const auto &created : rendered) QFile::remove(created);
                        return failure(result.value("error").toString());
                    }
                    rendered.append(output);
                }
#else
                const QString error = run(QStandardPaths::findExecutable("pdftoppm"),
                    {"-scale-to", "1600", "-png", "-f", "1", "-l", "20", path, stem}, cancel);
                if (!error.isEmpty())
                    return failure(error);
#endif
                const QStringList pages = QDir(directory).entryList(
                    {QFileInfo(stem).fileName() + "-*.png"}, QDir::Files, QDir::Name);
                if (pages.isEmpty()) return failure(QStringLiteral("The media utility produced no PDF pages."));
                attachedText += QStringLiteral("PDF rendered up to its first 20 pages.\n");
                for (const QString &page : pages) {
                    QFile image(QDir(directory).filePath(page));
                    if (!image.open(QIODevice::ReadOnly))
                        return failure(image.errorString());
                    content.append(QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{
                        {"url", "data:image/png;base64," + QString::fromLatin1(image.readAll().toBase64())}}}});
                }
            } else {
                attachedText += QStringLiteral("Binary file available for transfer; contents were not decoded.\n");
            }
        }
        preparedAttachments.append(attachment);
    }
    QString request = text;
    if (request.trimmed().isEmpty() && hasAudio)
        request = nativeAudio
            ? QStringLiteral("Respond to the speaker's request in the following audio.")
            : QStringLiteral("Respond to the speaker's request in the following voice-message transcription.");
    // Gemma's audio prompt format places the instruction before the audio.
    // Keep the clips last so trailing attachment metadata does not displace it.
    content.append(textPart(request + attachedText));
    for (const auto &audio : audioParts)
        content.append(audio);
    prepared = true;
    return {{"content", content.toVariantList()}, {"attachments", preparedAttachments},
        {"transcriptionInputs", transcriptionInputs}};
}
