#include "assistantmedia.h"
#include "../../../src/core/workspace/pilonworkspace.h"
#ifdef Q_OS_ANDROID
#include "../../../src/core/documents/documentpreview.h"
#endif

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QProcess>
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
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(3000))
        return process.errorString();
    int elapsed = 0;
    QByteArray errors;
    while (!process.waitForFinished(100)) {
        errors += process.readAllStandardError();
        errors = errors.right(8192);
        if (output)
            *output += process.readAllStandardOutput();
        else
            process.readAllStandardOutput();
        elapsed += 100;
        if (cancel->load() || elapsed > 600000) {
            process.kill();
            process.waitForFinished(3000);
            return cancel->load() ? QStringLiteral("Cancelled") : QStringLiteral("Media processing timed out.");
        }
    }
    errors += process.readAllStandardError();
    if (output)
        *output += process.readAllStandardOutput();
    return process.exitCode() == 0 && process.exitStatus() == QProcess::NormalExit
               ? QString{} : QString::fromUtf8(errors.right(8192));
}

QJsonObject textPart(const QString &text)
{
    return {{"type", "text"}, {"text", text}};
}
}

QVariantMap assistantAudioSummary(const QString &wavPath)
{
    QFile file(wavPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray wav = file.readAll();
    qsizetype offset = 12;
    QByteArray pcm;
    quint32 sampleRate = 16000;
    while (offset + 8 <= wav.size()) {
        const quint32 size = qFromLittleEndian<quint32>(wav.constData() + offset + 4);
        if (wav.mid(offset, 4) == "fmt " && size >= 16 && offset + 24 <= wav.size())
            sampleRate = qFromLittleEndian<quint32>(wav.constData() + offset + 12);
        if (wav.mid(offset, 4) == "data") {
            pcm = wav.mid(offset + 8, size);
            break;
        }
        offset += 8 + size + (size % 2);
    }
    const qsizetype samples = pcm.size() / 2;
    QVariantList levels;
    double maximum = 0.01;
    for (int bin = 0; bin < 48; ++bin) {
        const qsizetype first = samples * bin / 48;
        const qsizetype last = samples * (bin + 1) / 48;
        double squares = 0;
        for (qsizetype index = first; index < last; ++index) {
            const double sample = qFromLittleEndian<qint16>(pcm.constData() + index * 2) / 32768.0;
            squares += sample * sample;
        }
        const double level = std::sqrt(squares / qMax<qsizetype>(1, last - first));
        maximum = qMax(maximum, level);
        levels.append(level);
    }
    for (QVariant &level : levels)
        level = level.toDouble() / maximum;
    return {{"waveform", levels}, {"duration", samples / double(qMax(1u, sampleRate))}};
}

QVariantMap prepareAssistantMedia(const QVariantList &attachments, const QString &text,
                                 bool nativeAudio, const QString &directory,
                                 const QString &whisperExecutable, const QString &whisperModel,
                                 const std::shared_ptr<std::atomic_bool> &cancel)
{
    QJsonArray content;
    QJsonArray audioParts;
    QString attachedText;
    bool hasAudio = false;
    QVariantList preparedAttachments;
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
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            content.append(QJsonObject{{"type", "image_url"},
                {"image_url", QJsonObject{{"url", "data:image/png;base64," + QString::fromLatin1(png.toBase64())}}}});
        } else if (mime.startsWith("audio/")) {
#ifdef Q_OS_ANDROID
            const QString ffmpeg = PilonWorkspace::packagedExecutable(QStringLiteral("ffmpeg"));
#else
            const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
#endif
            hasAudio = true;
            const QString stem = QDir(directory).filePath(QUuid::createUuid().toString(QUuid::Id128));
            const QString wav = stem + ".wav";
            QString error = run(ffmpeg, {"-nostdin", "-v", "error", "-i", path, "-vn", "-ac", "1",
                                         "-ar", "16000", "-c:a", "pcm_s16le", wav}, cancel);
            if (!error.isEmpty())
                return failure(error);
            const QVariantMap summary = assistantAudioSummary(wav);
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
                for (const QString &chunk : chunks) {
                    QFile file(QDir(directory).filePath(chunk));
                    if (!file.open(QIODevice::ReadOnly))
                        return failure(file.errorString());
                    audioParts.append(QJsonObject{{"type", "input_audio"}, {"input_audio",
                        QJsonObject{{"data", QString::fromLatin1(file.readAll().toBase64())}, {"format", "wav"}}}});
                }
            }
            // Transcription is only for models without an audio encoder.
            if (!nativeAudio) {
                if (!QFileInfo::exists(whisperModel))
                    return failure(QStringLiteral("Download Speech recognition in Settings → Models to transcribe audio."));
                error = run(whisperExecutable, {"-m", whisperModel, "-f", wav, "-l", "auto",
                                               "-t", "8", "-nt", "-otxt", "-of", stem}, cancel);
                if (!error.isEmpty())
                    return failure(error);
                QFile transcript(stem + ".txt");
                if (!transcript.open(QIODevice::ReadOnly))
                    return failure(transcript.errorString());
                const QString speech = QString::fromUtf8(transcript.readAll()).trimmed();
                if (speech.isEmpty() || speech == "[BLANK_AUDIO]")
                    return failure(QStringLiteral("No intelligible speech was transcribed. Please try a clearer recording."));
                if (!speech.isEmpty() && speech != "[BLANK_AUDIO]")
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
            if (!error.isEmpty() || !ok)
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
            ? QStringLiteral("Transcribe the speech, then carry out the speaker's request.")
            : QStringLiteral("Respond to the speaker's request in the following voice-message transcription.");
    // Gemma's audio prompt format places the instruction before the audio.
    // Keep the clips last so trailing attachment metadata does not displace it.
    content.append(textPart(request + attachedText));
    for (const auto &audio : audioParts)
        content.append(audio);
    return {{"content", content.toVariantList()}, {"attachments", preparedAttachments}};
}
