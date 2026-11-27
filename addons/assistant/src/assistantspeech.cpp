#include "../../../src/core/workspace/pilonworkspace.h"
#include "assistantmanager.h"
#include "assistantmedia.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMediaDevices>
#include <QRegularExpression>
#include "../../../src/core/workspace/pilonsettings.h"
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextDocument>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <utility>
#ifdef Q_OS_UNIX
#include <csignal>
#endif

namespace {
const QString voiceSampleText = QStringLiteral("Hello, this is how I will sound in our conversations. What would you like to work on?");
const QString phononVoiceKey = QStringLiteral("assistant/phononVoice");
constexpr qint64 maxVoiceEmbeddingSize = 64 * 1024 * 1024;

QString phononVoiceDirectory()
{
    return qEnvironmentVariable("PILON_VOICE_DIR", PilonWorkspace::appDataDirectory() + "/assistant/voice");
}

QString voiceEmbeddingPath(const QString &input)
{
    const QUrl url(input);
    return url.isLocalFile() ? url.toLocalFile() : input;
}

bool validVoiceEmbedding(QFile &file)
{
    if (file.fileName().isEmpty()) return true;
    const QFileInfo info(file.fileName());
    if (!info.isAbsolute() || !info.isFile() || info.suffix().compare("safetensors", Qt::CaseInsensitive) != 0
        || !file.open(QIODevice::ReadOnly)) return false;
    const auto fileSize = file.size();
    if (fileSize < 12 || fileSize > maxVoiceEmbeddingSize) return false;
    const auto prefix = file.read(9);
    if (prefix.size() != 9 || prefix[8] != '{') return false;
    const auto size = qFromLittleEndian<quint64>(prefix.constData());
    if (size <= 2 || size > 1024 * 1024 || size >= quint64(fileSize - 8) || !file.seek(8)) return false;
    const auto header = QJsonDocument::fromJson(file.read(size)).object();
    const auto payloadSize = fileSize - 8 - qint64(size);
    const auto validTensor = [payloadSize](const QJsonObject &tensor) {
        const auto shape = tensor.value("shape").toArray();
        const auto offsets = tensor.value("data_offsets").toArray();
        if (tensor.value("dtype") != "F32" || shape.isEmpty() || offsets.size() != 2) return false;
        qint64 bytes = sizeof(float);
        for (const auto &value : shape) {
            const auto dimension = value.toInteger();
            if (dimension <= 0 || dimension > payloadSize / bytes) return false;
            bytes *= dimension;
        }
        const auto begin = offsets[0].toInteger(-1);
        const auto end = offsets[1].toInteger(-1);
        return begin >= 0 && end <= payloadSize && end > begin && end - begin == bytes;
    };
    const auto speaker = header.value("speaker_wavs").toObject();
    if (!speaker.isEmpty()) {
        const auto shape = speaker.value("shape").toArray();
        return (shape.size() == 2 || (shape.size() == 3 && shape[0].toInteger() == 1)) && validTensor(speaker);
    }
    static const QRegularExpression layer(R"(^transformer\.layers\.[0-9]+\.self_attn/cache$)");
    bool hasCache = false;
    for (auto it = header.constBegin(); it != header.constEnd(); ++it) {
        if (!layer.match(it.key()).hasMatch()) continue;
        const auto tensor = it.value().toObject();
        const auto shape = tensor.value("shape").toArray();
        const bool validShape = shape.size() == 4 || (shape.size() == 5 && shape[1].toInteger() == 1);
        if (!validShape || shape[0].toInteger() != 2 || !validTensor(tensor)) return false;
        hasCache = true;
    }
    return hasCache;
}

QString speechFfmpeg()
{
#ifdef Q_OS_ANDROID
    return PilonWorkspace::packagedExecutable("ffmpeg");
#else
    return QStandardPaths::findExecutable("ffmpeg");
#endif
}

bool saveAudioPreference(const QString &key, const QVariant &value)
{
    PilonSettings settings;
    const auto previous = settings.value(key);
    settings.setValue(key, value);
    settings.sync();
    if (settings.status() == PilonSettings::NoError) return true;
    if (previous.isValid()) settings.setValue(key, previous);
    else settings.remove(key);
    return false;
}

bool hasModelLanguage(const QVariantList &languages, const QString &id)
{
    return std::any_of(languages.cbegin(), languages.cend(), [&](const auto &item) {
        return item.toMap().value("id") == id;
    });
}

void discardSpeechOutput(const QJsonObject &request)
{
    const auto output = request.value("output").toString();
    const auto reference = request.value("reference").toObject().value("path").toString();
    QStringList paths;
    if (!output.isEmpty()) {
        paths = {output, output + ".partial.wav", output + ".render.wav", output + ".render.wav.partial.wav", output + ".tempo.wav",
            QFileInfo(output).absolutePath() + '/' + QFileInfo(output).completeBaseName() + ".tempo.wav"};
    }
    const auto candidate = request.value("reference_output").toString();
    if (!candidate.isEmpty()) paths << candidate << candidate + ".partial.wav";
    for (const auto &path : paths)
        if (path != reference) QFile::remove(path);
}
}

QVariantMap AssistantManager::activeAudioModel(const QString &kind, const QString &preferred) const
{
    QVariantMap fallback;
    for (const auto &entry : audioModels()) {
        const auto model = entry.toMap();
        if (model.value("kind") != kind || !model.value("installed").toBool()) continue;
        if (model.value("id") == preferred) return model;
        if (fallback.isEmpty()) fallback = model;
    }
    return fallback;
}

QString AssistantManager::speechInputModel() const
{
    return activeAudioModel("input", m_speechInputModel).value("id").toString();
}

QString AssistantManager::selectedSpeechInputPath() const
{
    return activeAudioModel("input", m_speechInputModel).value("weights").toString();
}

QString AssistantManager::speechOutputModel() const
{
    return activeAudioModel("output", m_speechOutputModel).value("id").toString();
}

bool AssistantManager::speechInputAvailable() const
{
    return !selectedSpeechInputPath().isEmpty() && QFileInfo(runtimePath("whisper-cli")).isExecutable();
}

QString AssistantManager::nativeSpeechWorker() const
{
#ifdef Q_OS_ANDROID
    return PilonWorkspace::packagedExecutable("speech");
#else
    const QDir runtime(PilonWorkspace::runtimeDirectory());
    const auto worker = runtime.filePath("speech/bin/pilon-speech");
    return QFileInfo(worker).isExecutable() ? worker : runtime.filePath("speech/bin/pilon-breeze");
#endif
}

bool AssistantManager::speechOutputAvailable() const
{
    const auto model = activeAudioModel("output", m_speechOutputModel);
    if (model.value("weights").toString().isEmpty()) return false;
    const QFileInfo worker(nativeSpeechWorker());
#ifndef Q_OS_ANDROID
    if (!QFileInfo(worker.dir().filePath("libaudiocpp.so.0")).isFile()) return false;
#endif
    const auto engine = model.value("engine").toString();
    return worker.isExecutable() && (engine == "supertonic" || engine == "gradium_phonon"
        || (engine == "breeze_tts" && !speechFfmpeg().isEmpty()));
}

bool AssistantManager::selectAudioModel(const QString &id)
{
    if (busy() || recording() || m_speaking) return false;
    for (const auto &entry : audioModels()) {
        const auto model = entry.toMap();
        if (model.value("id") != id || !model.value("installed").toBool()) continue;
        const bool input = model.value("kind") == "input";
        auto &selected = input ? m_speechInputModel : m_speechOutputModel;
        if (!saveAudioPreference(input ? "assistant/speechInput" : "assistant/speechOutput", id)) {
            fail(tr("Unable to save the audio model selection."));
            return false;
        }
        if (!input && id != speechOutputModel()) {
            stopSpeech();
            stopSpeechProcess();
        }
        selected = id;
        emit audioModelsChanged();
        return true;
    }
    return false;
}

QVariantMap AssistantManager::validSpeechPreset(const QString &preset, const QString &language, int speed) const
{
    const auto model = activeAudioModel("output", m_speechOutputModel);
    if (!model.value("presets").toStringList().contains(preset) || speed < 1 || speed > 10) return {};
    const auto languages = model.value("languages").toList();
    if (!hasModelLanguage(languages, language)) return {};
    return {{"preset", preset}, {"language", language}, {"speed", speed}};
}

bool AssistantManager::saveSpeechPreset(const QString &preset, const QString &language, int speed)
{
    if (busy() || recording() || m_speaking) return false;
    const auto selected = validSpeechPreset(preset, language, speed);
    if (selected.isEmpty()) return false;
    if (!saveAudioPreference("assistant/speechPreset", selected)) {
        fail(tr("Unable to save the voice settings."));
        return false;
    }
    m_speechPreset = selected;
    emit voiceSettingsChanged();
    return true;
}

void AssistantManager::previewSpeechPreset(const QString &preset, const QString &language, int speed)
{
    if (!m_active || busy() || recording() || m_speaking) return;
    const auto selected = validSpeechPreset(preset, language, speed);
    if (selected.isEmpty()) return;
    emit speechStopped();
    m_speechMessageIndex = -1;
    m_speechPreview = true;
    m_speechRequest = QJsonObject::fromVariantMap(selected);
    const auto model = activeAudioModel("output", m_speechOutputModel);
    if (model.value("streaming").toBool()) m_speechRequest.insert("stream", true);
    m_speechRequest.insert("text", language == "en" ? voiceSampleText : QStringLiteral("1, 2, 3."));
    m_speechRequest.insert("output", m_sessionDirectory.path() + "/voice-preview-" + QUuid::createUuid().toString(QUuid::Id128) + ".wav");
    generateSpeech();
}

QByteArray AssistantManager::stereoSpeechPcm(const QByteArray &mono)
{
    Q_ASSERT(mono.size() % 2 == 0);
    QByteArray stereo(mono.size() * 2, Qt::Uninitialized);
    auto *output = stereo.data();
    for (qsizetype offset = 0; offset < mono.size(); offset += 2) {
        output[0] = output[2] = mono.at(offset);
        output[1] = output[3] = mono.at(offset + 1);
        output += 4;
    }
    return stereo;
}

void AssistantManager::receiveSpeechPcm(const QJsonObject &packet)
{
    if (!m_speaking || !m_speechRequest.value("stream").toBool() || m_speechPcmUnavailable) return;
    const auto rate = packet.value("rate").toInt();
    const auto channels = packet.value("channels").toInt();
    const auto encoded = packet.value("pcm").toString();
    if (rate < 8000 || rate > 96000 || channels < 1 || channels > 2 || encoded.size() > 22000) return;
    auto pcm = QByteArray::fromBase64Encoding(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!pcm || pcm.decoded.isEmpty() || pcm.decoded.size() % (2 * channels) != 0) return;
    if (!m_speechAudio) {
        QAudioFormat format;
        format.setSampleRate(rate);
        format.setSampleFormat(QAudioFormat::Int16);
        format.setChannelConfig(QAudioFormat::ChannelConfigStereo);
        const auto device = QMediaDevices::defaultAudioOutput();
        // Center mono speech explicitly on a stereo output. Some sinks map mono to the left channel.
        if (channels == 1 && !device.isFormatSupported(format))
            format.setChannelConfig(QAudioFormat::ChannelConfigMono);
        if (!device.isFormatSupported(format)) {
            m_speechPcmUnavailable = true;
            return;
        }
        m_speechPcmSourceChannels = channels;
        m_speechAudio = std::make_unique<QAudioSink>(device, format);
        m_speechAudio->setBufferSize(format.bytesForDuration(200000));
        m_speechAudioWriter = m_speechAudio->start();
        m_speechPlaybackTimer.start();
    }
    if (m_speechAudio->format().sampleRate() != rate || m_speechPcmSourceChannels != channels) return;
    const auto samples = channels == 1 && m_speechAudio->format().channelCount() == 2
        ? stereoSpeechPcm(pcm.decoded) : std::move(pcm.decoded);
    if (m_speechPcm.size() + samples.size() < 64 * 1024 * 1024) m_speechPcm += samples;
    else m_speechPcmUnavailable = true;
    writeSpeechPcm();
}

void AssistantManager::writeSpeechPcm()
{
    if (!m_speechAudio) return;
    bool failed = !m_speechAudioWriter || m_speechAudio->state() == QtAudio::StoppedState || m_speechPcmUnavailable;
    if (!failed && !m_speechPcm.isEmpty()) {
        const auto frame = m_speechAudio->format().bytesPerFrame();
        const auto length = std::min(m_speechAudio->bytesFree(), m_speechPcm.size()) / frame * frame;
        if (length > 0) {
            const auto written = m_speechAudioWriter->write(m_speechPcm.constData(), length);
            failed = written < 0;
            if (written > 0) {
                m_speechPcmPlayed = true;
                m_speechPcm.remove(0, written);
            }
        }
    }
    if (!failed && (!m_speechPcmFinished || !m_speechPcm.isEmpty() || m_speechAudio->state() != QtAudio::IdleState))
        return;
    const bool played = m_speechPcmPlayed;
    if (!failed) m_speechAudio->stop();
    resetSpeechPlayback();
    m_speechPcmPlayed = played;
    m_speechPcmUnavailable = failed;
    if (m_speaking && m_speechRequest.isEmpty()) {
        m_speaking = false;
        m_speechPreview = false;
        emit optionsChanged();
        emit stateChanged();
    }
}

void AssistantManager::resetSpeechPlayback()
{
    m_speechPlaybackTimer.stop();
    if (m_speechAudio) m_speechAudio->reset();
    m_speechAudio.reset();
    m_speechAudioWriter = nullptr;
    m_speechPcm.clear();
    m_speechPcmSourceChannels = 0;
    m_speechPcmFinished = m_speechPcmPlayed = m_speechPcmUnavailable = false;
}

bool AssistantManager::savePhononVoiceSettings(const QString &voiceEmbedding, double temperature)
{
    if (busy() || recording() || m_speaking || !std::isfinite(temperature) || temperature <= 0 || temperature > 1
        || activeAudioModel("output", m_speechOutputModel).value("engine") != "gradium_phonon") return false;
    const auto source = voiceEmbeddingPath(voiceEmbedding);
    QFile input(source);
    if (!validVoiceEmbedding(input)) {
        fail(tr("Choose a readable Phonon voice embedding file."));
        return false;
    }
    const auto oldPath = m_phononVoice.value("voiceEmbedding").toString();
    auto path = source;
    auto name = m_phononVoice.value("voiceName").toString();
    if (source != oldPath) {
        name = QFileInfo(source).fileName();
        if (!source.isEmpty()) {
            const auto directory = phononVoiceDirectory();
            path = directory + "/phonon-" + QUuid::createUuid().toString(QUuid::Id128) + ".safetensors";
            QSaveFile output(path);
            if (!QDir().mkpath(directory) || !input.seek(0) || !output.open(QIODevice::WriteOnly)) {
                fail(tr("Unable to save the voice embedding."));
                return false;
            }
            output.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            const auto bytes = input.read(maxVoiceEmbeddingSize + 1);
            if (bytes.size() > maxVoiceEmbeddingSize || bytes.size() != input.size()
                || output.write(bytes) != bytes.size() || !output.commit()) {
                fail(tr("Unable to save the voice embedding."));
                return false;
            }
        }
    }
    const QVariantMap settings{{"temperature", temperature}, {"voiceEmbedding", path}, {"voiceName", name}};
    if (!saveAudioPreference(phononVoiceKey, settings)) {
        if (!path.isEmpty() && path != source) QFile::remove(path);
        fail(tr("Unable to save the voice settings."));
        return false;
    }
    if (path != oldPath && QFileInfo(oldPath).absolutePath() == phononVoiceDirectory()
        && QFileInfo(oldPath).fileName().startsWith("phonon-")) QFile::remove(oldPath);
    m_phononVoice = settings;
    m_error.clear();
    emit voiceSettingsChanged();
    emit stateChanged();
    return true;
}

void AssistantManager::previewPhononVoice(const QString &voiceEmbedding, double temperature)
{
    if (!m_active || busy() || recording() || m_speaking || !std::isfinite(temperature) || temperature <= 0 || temperature > 1)
        return;
    const auto path = voiceEmbeddingPath(voiceEmbedding);
    QFile input(path);
    if (!validVoiceEmbedding(input)) {
        fail(tr("Choose a readable Phonon voice embedding file."));
        return;
    }
    emit speechStopped();
    m_speechMessageIndex = -1;
    m_speechPreview = true;
    m_speechRequest = {{"text", voiceSampleText}, {"voiceEmbedding", path}, {"temperature", temperature},
        {"output", m_sessionDirectory.path() + "/voice-preview-" + QUuid::createUuid().toString(QUuid::Id128) + ".wav"}};
    const auto model = activeAudioModel("output", m_speechOutputModel);
    if (model.value("streaming").toBool()) m_speechRequest.insert("stream", true);
    generateSpeech();
}

void AssistantManager::initializeSpeech()
{
#ifdef Q_OS_UNIX
    m_speechProcess.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
#endif
    m_voiceMode = PilonSettings().value("assistant/voiceMode", false).toBool();
    m_voicePrompt = PilonSettings().value("assistant/voicePrompt",
        "A calm adult man with a warm, clear, natural voice. Speak conversationally at a relaxed but concise pace.").toString();
    m_voiceSpeed = qBound(1, PilonSettings().value("assistant/voiceSpeed", 5).toInt(), 10);
    m_voiceReference = QJsonObject::fromVariantMap(PilonSettings().value("assistant/voiceReference").toMap());
    m_speechInputModel = PilonSettings().value("assistant/speechInput").toString();
    m_speechOutputModel = PilonSettings().value("assistant/speechOutput").toString();
    m_speechPreset = PilonSettings().value("assistant/speechPreset",
        QVariantMap{{"preset", "M1"}, {"language", "en"}, {"speed", 5}}).toMap();
    m_phononVoice = PilonSettings().value(phononVoiceKey, QVariantMap{{"temperature", 0.3}}).toMap();
    m_speechPlaybackTimer.setInterval(10);
    connect(&m_speechPlaybackTimer, &QTimer::timeout, this, &AssistantManager::writeSpeechPcm);
    connect(&m_models, &AssistantModels::installedChanged, this, [this] {
        if (!m_speechStopping && !m_speechProcessModel.isEmpty() && m_speechProcessModel != speechOutputModel()) {
            stopSpeech();
            stopSpeechProcess();
        }
        emit audioModelsChanged();
    });
    connect(this, &AssistantManager::audioModelsChanged, this, &AssistantManager::optionsChanged);
    connect(&m_speechProcess, &QProcess::readyReadStandardError, this, [this] {
        m_speechLog = (m_speechLog + m_speechProcess.readAllStandardError()).right(4096);
    });
    connect(&m_speechProcess, &QProcess::readyReadStandardOutput, this, [this] {
        const auto bytes = m_speechProcess.readAllStandardOutput();
        if (m_speechStopping) return;
        m_speechBuffer += bytes;
        while (m_speechBuffer.contains('\n')) {
            const auto end = m_speechBuffer.indexOf('\n');
            const auto data = QJsonDocument::fromJson(m_speechBuffer.left(end)).object();
            m_speechBuffer.remove(0, end + 1);
            if (data.value("ready").toBool()) {
                m_speechReady = true;
                startSpeechRequest();
            } else if (data.contains("error")) {
                speechFailed(data.value("error").toString());
            } else if (data.contains("pcm")) {
                receiveSpeechPcm(data);
            } else if (data.contains("output") && m_speaking) {
                const QString path = data.value("output").toString();
                if (path != m_speechRequest.value("output").toString() || !QFileInfo(path).isFile())
                    continue;
                const bool preview = m_speechPreview;
                if (m_speechRequest.contains("instruction")) {
                    auto voice = data.value("reference").toObject();
                    const auto expected = m_speechRequest.contains("reference")
                        ? m_speechRequest.value("reference").toObject()
                        : QJsonObject{{"path", m_speechRequest.value("reference_output")},
                                      {"text", m_speechRequest.value("reference_text")}};
                    if (voice.value("path") != expected.value("path")
                        || voice.value("text") != expected.value("text") || voice.isEmpty()) {
                        speechFailed(tr("Breeze returned an invalid voice sample."));
                        continue;
                    }
                    const auto prompt = m_speechRequest.value("instruction").toString();
                    voice.insert("prompt", prompt);
                    if (preview) m_previewVoice = voice;
                    if (m_saveVoice || (!preview && m_voiceReference.isEmpty() && prompt == m_voicePrompt)) {
                        if (!persistVoice(prompt, m_saveVoice ? m_pendingVoiceSpeed : m_voiceSpeed, voice)) continue;
                    }
                }
                m_speechPcmFinished = true;
                writeSpeechPcm();
                const bool streamed = m_speechPcmPlayed;
                m_speechRequest = {};
                m_speaking = bool(m_speechAudio);
                m_speechPreview = preview && m_speaking;
                m_saveVoice = false;
                if (!preview && m_speechMessageIndex >= 0 && m_speechMessageIndex < m_messages.size()) {
                    QVariantMap attachment = assistantAudioSummary(path);
                    attachment.insert("name", tr("Spoken reply"));
                    attachment.insert("mime", "audio/wav");
                    attachment.insert("url", QUrl::fromLocalFile(path));
                    attachment.insert("bytes", QFileInfo(path).size());
                    auto message = m_messages[m_speechMessageIndex].toMap();
                    message.insert("attachments", QVariantList{attachment});
                    m_messages[m_speechMessageIndex] = message;
                    emit messagesChanged();
                    saveChat();
                }
                emit optionsChanged();
                emit stateChanged();
                if (!streamed) {
                    if (preview) emit voicePreviewReady(QUrl::fromLocalFile(path));
                    else emit speechReady(QUrl::fromLocalFile(path));
                } else if (preview) {
                    QFile::remove(path);
                }
            }
        }
    });
    connect(&m_speechProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_speechProcessModel.clear();
            if (std::exchange(m_speechStopping, false)) {
                discardSpeechOutput(std::exchange(m_cancelledSpeechRequest, {}));
                if (m_speaking) generateSpeech();
                return;
            }
#ifdef Q_OS_ANDROID
            speechFailed(tr("The packaged speech runtime is missing or cannot start. Reinstall Pilon."));
#else
            speechFailed(tr("Unable to start the local speech runtime."));
#endif
        }
    });
    connect(&m_speechProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        m_speechReady = false;
        m_speechProcessModel.clear();
        if (std::exchange(m_speechStopping, false)) {
            discardSpeechOutput(std::exchange(m_cancelledSpeechRequest, {}));
            if (m_speaking) generateSpeech();
            return;
        }
        if (!m_speechRequest.isEmpty()) {
            speechFailed(tr("Speech runtime stopped (%1): %2").arg(code).arg(QString::fromUtf8(m_speechLog)));
        }
    });
}

void AssistantManager::setVoiceMode(bool enabled)
{
    m_voiceMode = enabled;
    PilonSettings().setValue("assistant/voiceMode", enabled);
    if (!enabled)
        stopSpeech();
    emit optionsChanged();
}

void AssistantManager::setVoicePrompt(const QString &prompt)
{
    saveVoiceSettings(prompt, m_voiceSpeed);
}

void AssistantManager::setVoiceSpeed(int speed)
{
    saveVoiceSettings(m_voicePrompt, speed);
}

bool AssistantManager::saveVoiceSettings(const QString &prompt, int speed)
{
    const QString value = prompt.trimmed().left(2000);
    if (value.isEmpty() || value.contains(QChar::Null) || speed < 1 || speed > 10)
        return false;
    const auto reference = voiceReference(value);
    if (!reference.isEmpty() && QFileInfo(reference.value("path").toString()).isFile()) {
        stopSpeech();
        return persistVoice(value, speed, reference);
    }
    // Save the exact sample being previewed, once generation succeeds.
    if (m_speechPreview && m_speechRequest.value("instruction") != value)
        stopSpeech();
    if (!m_speaking)
        previewVoice(value, speed);
    if (m_speechPreview && m_speechRequest.value("instruction") == value) {
        m_saveVoice = true;
        m_pendingVoiceSpeed = speed;
        return true;
    }
    return false;
}

QJsonObject AssistantManager::voiceReference(const QString &prompt) const
{
    if (m_previewVoice.value("prompt") == prompt)
        return m_previewVoice;
    if (m_voiceReference.value("prompt") == prompt)
        return m_voiceReference;
    return {};
}

void AssistantManager::prepareVoiceReference(const QJsonObject &reference)
{
    if (!reference.isEmpty()) {
        m_speechRequest.insert("reference", reference);
    } else {
        m_speechRequest.insert("reference_output", m_sessionDirectory.path() + "/voice-"
            + QUuid::createUuid().toString(QUuid::Id128) + ".wav");
        m_speechRequest.insert("reference_text", voiceSampleText);
    }
}

bool AssistantManager::persistVoice(const QString &prompt, int speed, const QJsonObject &reference)
{
    auto saved = reference;
    saved.insert("prompt", prompt);
    const QString source = reference.value("path").toString();
    QString destination = source;
    if (source != m_voiceReference.value("path").toString()) {
        const QString root = qEnvironmentVariable("PILON_VOICE_DIR",
            PilonWorkspace::appDataDirectory() + "/assistant/voice");
        if (!QDir().mkpath(root)) {
            speechFailed(tr("Unable to create the saved voice directory."));
            return false;
        }
        QFile::setPermissions(root, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QFile input(source);
        if (reference.value("text").toString().isEmpty() || !input.open(QIODevice::ReadOnly)
            || input.size() < 44 || input.size() > 16 * 1024 * 1024) {
            speechFailed(tr("Unable to read the voice sample. Preview it again before saving."));
            return false;
        }
        destination = root + '/' + QUuid::createUuid().toString(QUuid::Id128) + ".wav";
        QSaveFile output(destination);
        const auto audio = input.readAll();
        if (!output.open(QIODevice::WriteOnly)) {
            speechFailed(tr("Unable to save the voice sample: %1").arg(output.errorString()));
            return false;
        }
        output.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        if (output.write(audio) != audio.size() || !output.commit()) {
            speechFailed(tr("Unable to save the voice sample: %1").arg(output.errorString()));
            return false;
        }
        saved.insert("path", destination);
    }
    PilonSettings settings;
    const QStringList keys{"assistant/voicePrompt", "assistant/voiceSpeed", "assistant/voiceReference"};
    QVariantList previous;
    for (const auto &key : keys)
        previous.append(settings.value(key));
    settings.setValue(keys[0], prompt);
    settings.setValue(keys[1], speed);
    settings.setValue(keys[2], saved.toVariantMap());
    settings.sync();
    if (settings.status() != PilonSettings::NoError) {
        for (int i = 0; i < keys.size(); ++i) {
            if (previous[i].isValid()) settings.setValue(keys[i], previous[i]);
            else settings.remove(keys[i]);
        }
        if (destination != source)
            QFile::remove(destination);
        speechFailed(tr("Unable to save the voice settings."));
        return false;
    }
    m_voicePrompt = prompt;
    m_voiceSpeed = speed;
    m_voiceReference = saved;
    m_previewVoice = saved;
    m_error.clear();
    emit voiceSettingsChanged();
    emit stateChanged();
    return true;
}

void AssistantManager::previewVoice(const QString &prompt, int speed)
{
    if (!m_active) { fail(tr("Enable Assistant to preview a voice.")); return; }
    const auto engine = activeAudioModel("output", m_speechOutputModel).value("engine").toString();
    if (engine == "gradium_phonon") {
        previewPhononVoice(m_phononVoice.value("voiceEmbedding").toString(),
                           m_phononVoice.value("temperature", 0.3).toDouble());
        return;
    }
    const QString value = prompt.trimmed().left(2000);
    if (m_busy || recording() || m_speaking) return;
    if (value.isEmpty() || value.contains(QChar::Null) || speed < 1 || speed > 10)
        return;
    emit speechStopped();
    m_speechMessageIndex = -1;
    m_speechPreview = true;
    m_speechRequest = {{"text", voiceSampleText},
        {"output", m_sessionDirectory.path() + "/voice-preview-" + QUuid::createUuid().toString(QUuid::Id128) + ".wav"}};
    m_speechRequest.insert("preview", true);
    m_speechRequest.insert("instruction", value);
    m_speechRequest.insert("speed", speed);
    auto reference = voiceReference(value);
    if (!QFileInfo(reference.value("path").toString()).isFile())
        reference = {}; // Explicit Preview can replace a missing sample; replies never do.
    prepareVoiceReference(reference);
    generateSpeech();
}

void AssistantManager::speakReply(const QString &text)
{
    if (!speechOutputAvailable()) return;
    QTextDocument document;
    document.setMarkdown(text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub)
                               | QTextDocument::MarkdownNoHTML);
    const QString plain = document.toPlainText().trimmed();
    if (plain.isEmpty())
        return;
    m_speechMessageIndex = m_assistantIndex;
    m_speechPreview = false;
    const auto model = activeAudioModel("output", m_speechOutputModel);
    const auto engine = model.value("engine").toString();
    if (m_speaking) stopSpeech();
    else emit speechStopped();
    m_speechRequest = {};
    if (engine == "supertonic") {
        const auto presets = model.value("presets").toStringList();
        const auto languages = model.value("languages").toList();
        const auto savedPreset = m_speechPreset.value("preset").toString();
        const auto savedLanguage = m_speechPreset.value("language").toString();
        const auto preset = presets.contains(savedPreset) ? savedPreset : presets.value(0);
        const auto language = hasModelLanguage(languages, savedLanguage) ? savedLanguage
            : languages.value(0).toMap().value("id").toString();
        const auto savedSpeed = m_speechPreset.value("speed").toInt();
        const auto speed = savedSpeed >= 1 && savedSpeed <= 10 ? savedSpeed : 5;
        m_speechRequest = {{"preset", preset}, {"language", language}, {"speed", speed}};
    } else if (engine == "breeze_tts") {
        m_speechRequest = {{"instruction", m_voicePrompt}, {"speed", m_voiceSpeed}};
        prepareVoiceReference(m_voiceReference);
    } else if (engine == "gradium_phonon") {
        m_speechRequest = {{"temperature", m_phononVoice.value("temperature", 0.3).toDouble()},
                           {"voiceEmbedding", m_phononVoice.value("voiceEmbedding").toString()}};
    }
    if (model.value("streaming").toBool()) m_speechRequest.insert("stream", true);
    m_speechRequest.insert("text", plain);
    m_speechRequest.insert("output", attachmentDirectory() + "/speech-" + QUuid::createUuid().toString(QUuid::Id128) + ".wav");
    generateSpeech();
}

void AssistantManager::speechFailed(const QString &error)
{
    resetSpeechPlayback();
    discardSpeechOutput(m_speechRequest);
    m_error = error;
    m_speaking = false;
    m_speechPreview = false;
    m_saveVoice = false;
    m_speechRequest = {};
    emit stateChanged();
    emit optionsChanged();
}

void AssistantManager::generateSpeech()
{
    m_error.clear();
    if (!m_speaking) resetSpeechPlayback();
    m_speaking = true;
    emit stateChanged();
    emit optionsChanged();
    if (m_speechStopping) return;
    const auto model = activeAudioModel("output", m_speechOutputModel);
    if (model.isEmpty()) {
        speechFailed(tr("Download a speech output model in Audio models.")); return;
    }
    const auto engine = model.value("engine").toString();
    const auto requestedEngine = m_speechRequest.contains("instruction") ? "breeze_tts"
        : m_speechRequest.contains("preset") ? "supertonic" : "gradium_phonon";
    if (engine != requestedEngine) {
        speechFailed(tr("The voice settings do not match the selected speech output model.")); return;
    }
    const auto modelId = model.value("id").toString();
    if (m_speechProcess.state() != QProcess::NotRunning) {
        if (m_speechProcessModel != modelId) stopSpeechProcess();
        else if (m_speechReady) startSpeechRequest();
        return;
    }
    m_speechLog.clear();
    m_speechBuffer.clear();
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto runtime = nativeSpeechWorker();
    const auto ffmpeg = engine == "gradium_phonon" ? QString{} : speechFfmpeg();
    const auto weights = model.value("weights").toString();
    if (!QFileInfo(runtime).isExecutable() || weights.isEmpty() || (engine == "breeze_tts" && ffmpeg.isEmpty())) {
        speechFailed(tr("The local speech runtime is unavailable.")); return;
    }
    // audio.cpp extracts GGUF sidecars under TMPDIR.
    environment.insert("TMPDIR", m_sessionDirectory.path());
#ifdef Q_OS_ANDROID
    environment.insert("LD_LIBRARY_PATH", QFileInfo(runtime).absolutePath());
#endif
    m_speechProcessModel = modelId;
    m_speechProcess.setProcessEnvironment(environment);
    m_speechProcess.start(runtime, {weights, ffmpeg, "4", engine});
}

void AssistantManager::startSpeechRequest()
{
    if (m_speechRequest.isEmpty() || !m_speaking)
        return;
    m_speechProcess.write(QJsonDocument(m_speechRequest).toJson(QJsonDocument::Compact) + '\n');
}

void AssistantManager::stopSpeech()
{
    const bool wasGenerating = m_speaking;
    resetSpeechPlayback();
    m_speaking = false;
    m_speechPreview = false;
    m_saveVoice = false;
    const auto request = m_speechRequest;
    m_speechRequest = {};
    if (wasGenerating || !voiceMode()) {
        if (!m_speechStopping && m_speechProcess.state() != QProcess::NotRunning)
            m_cancelledSpeechRequest = request;
        stopSpeechProcess();
    }
    discardSpeechOutput(request);
    emit optionsChanged();
    emit stateChanged();
    emit speechStopped();
}

void AssistantManager::stopSpeechProcess()
{
    m_speechReady = false;
    m_speechBuffer.clear();
    if (m_speechStopping || m_speechProcess.state() == QProcess::NotRunning) return;
    m_speechStopping = true;
#ifdef Q_OS_UNIX
    const auto process = m_speechProcess.processId();
    if (process > 0) ::kill(-process, SIGKILL);
#endif
    m_speechProcess.kill();
}
