#include "../../../src/core/workspace/pilonworkspace.h"
#include "assistantmanager.h"
#include "assistantmedia.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include "../../../src/core/workspace/pilonsettings.h"
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextDocument>
#include <QUuid>
#ifdef Q_OS_UNIX
#include <csignal>
#endif

namespace {
const QString voiceSampleText = QStringLiteral("Hello, this is how I will sound in our conversations. What would you like to work on?");
void discardSpeechOutput(const QJsonObject &request)
{
    const auto output = request.value("output").toString();
    const auto reference = request.value("reference").toObject().value("path").toString();
    QStringList paths;
    if (!output.isEmpty()) {
        paths = {output, output + ".render.wav", output + ".render.wav.partial.wav", output + ".tempo.wav",
            QFileInfo(output).absolutePath() + '/' + QFileInfo(output).completeBaseName() + ".tempo.wav"};
    }
    const auto candidate = request.value("reference_output").toString();
    if (!candidate.isEmpty()) paths << candidate << candidate + ".partial.wav";
    for (const auto &path : paths)
        if (path != reference) QFile::remove(path);
}
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
    connect(&m_speechProcess, &QProcess::readyReadStandardError, this, [this] {
        m_speechLog = (m_speechLog + m_speechProcess.readAllStandardError()).right(4096);
    });
    connect(&m_speechProcess, &QProcess::readyReadStandardOutput, this, [this] {
        m_speechBuffer += m_speechProcess.readAllStandardOutput();
        while (m_speechBuffer.contains('\n')) {
            const auto end = m_speechBuffer.indexOf('\n');
            const auto data = QJsonDocument::fromJson(m_speechBuffer.left(end)).object();
            m_speechBuffer.remove(0, end + 1);
            if (data.value("ready").toBool()) {
                m_speechReady = true;
                startSpeechRequest();
            } else if (data.contains("error")) {
                speechFailed(tr("Breeze: %1").arg(data.value("error").toString()));
            } else if (data.contains("output") && m_speaking) {
                const QString path = data.value("output").toString();
                if (path != m_speechRequest.value("output").toString() || !QFileInfo(path).isFile())
                    continue;
                const bool preview = m_speechPreview;
                const auto reference = data.value("reference").toObject();
                const auto expected = m_speechRequest.contains("reference")
                    ? m_speechRequest.value("reference").toObject()
                    : QJsonObject{{"path", m_speechRequest.value("reference_output")},
                                  {"text", m_speechRequest.value("reference_text")}};
                if (reference.value("path") != expected.value("path")
                    || reference.value("text") != expected.value("text") || reference.isEmpty()) {
                    speechFailed(tr("Breeze returned an invalid voice sample."));
                    continue;
                }
                auto voice = reference;
                const auto prompt = m_speechRequest.value("instruction").toString();
                voice.insert("prompt", prompt);
                if (preview)
                    m_previewVoice = voice;
                if (m_saveVoice || (!preview && m_voiceReference.isEmpty() && prompt == m_voicePrompt)) {
                    if (!persistVoice(prompt, m_saveVoice ? m_pendingVoiceSpeed : m_voiceSpeed, voice))
                        continue;
                }
                m_speechRequest = {};
                m_speaking = false;
                m_speechPreview = false;
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
                if (preview)
                    emit voicePreviewReady(QUrl::fromLocalFile(path));
                else
                    emit speechReady(QUrl::fromLocalFile(path));
            }
        }
    });
    connect(&m_speechProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
#ifdef Q_OS_ANDROID
            speechFailed(tr("The packaged Breeze runtime is missing or cannot start. Reinstall Pilon."));
#else
            speechFailed(tr("Unable to start Breeze. Run addons/assistant/scripts/setup_breeze.sh first."));
#endif
        }
    });
    connect(&m_speechProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        m_speechReady = false;
        if (m_speaking) {
            speechFailed(tr("Breeze stopped (%1): %2").arg(code).arg(QString::fromUtf8(m_speechLog)));
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
    const QString value = prompt.trimmed().left(2000);
    if (m_busy || recording() || m_speaking || value.isEmpty() || value.contains(QChar::Null)
        || speed < 1 || speed > 10)
        return;
    emit speechStopped();
    m_speechMessageIndex = -1;
    m_speechPreview = true;
    m_speechRequest = {{"text", voiceSampleText}, {"preview", true},
        {"instruction", value}, {"speed", speed},
        {"output", m_sessionDirectory.path() + "/voice-preview-" + QUuid::createUuid().toString(QUuid::Id128) + ".wav"}};
    auto reference = voiceReference(value);
    if (!QFileInfo(reference.value("path").toString()).isFile())
        reference = {}; // Explicit Preview can replace a missing sample; replies never do.
    prepareVoiceReference(reference);
    generateSpeech();
}

void AssistantManager::speakReply(const QString &text)
{
    QTextDocument document;
    document.setMarkdown(text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub)
                               | QTextDocument::MarkdownNoHTML);
    const QString plain = document.toPlainText().trimmed();
    if (plain.isEmpty())
        return;
    m_speechMessageIndex = m_assistantIndex;
    m_speechPreview = false;
    m_speechRequest = {{"text", plain}, {"instruction", m_voicePrompt},
        {"speed", m_voiceSpeed},
        {"output", attachmentDirectory() + "/speech-" + QUuid::createUuid().toString(QUuid::Id128) + ".wav"}};
    prepareVoiceReference(m_voiceReference);
    generateSpeech();
}

void AssistantManager::speechFailed(const QString &error)
{
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
    m_speaking = true;
    emit stateChanged();
    emit optionsChanged();
    if (m_speechProcess.state() == QProcess::Running && m_speechReady) {
        startSpeechRequest();
        return;
    }
    m_speechLog.clear();
    m_speechBuffer.clear();
#ifdef Q_OS_ANDROID
    const QString runtime = PilonWorkspace::packagedExecutable("breeze");
    const QString ffmpeg = PilonWorkspace::packagedExecutable("ffmpeg");
    const QString model = modelRoot() + "/breeze-tts-2/breeze-tts-2-q8_0.gguf";
    if (runtime.isEmpty() || ffmpeg.isEmpty()) {
        speechFailed(tr("The packaged speech runtime is missing. Reinstall Pilon.")); return;
    }
    if (!QFileInfo(model).isFile()) {
        speechFailed(tr("Download Speech output in Models first.")); return;
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("LD_LIBRARY_PATH", QFileInfo(runtime).absolutePath());
    m_speechProcess.setProcessEnvironment(environment);
    m_speechProcess.start(runtime, {model, ffmpeg, "4"});
#else
    const QString root = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("..");
    const QString runtime = qEnvironmentVariable("PILON_BREEZE_PYTHON", root + "/runtime/breeze/bin/python");
    const QString worker = qEnvironmentVariable("PILON_BREEZE_WORKER", root + "/addons/assistant/workers/breeze_worker.py");
    m_speechProcess.start(runtime, {"-u", worker, modelRoot() + "/breeze-tts-2",
                                   QFileInfo(worker).absolutePath() + "/../vendor/breeze"});
#endif
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
    m_speaking = false;
    m_speechPreview = false;
    m_saveVoice = false;
    const auto request = m_speechRequest;
    m_speechRequest = {};
    if (wasGenerating || !m_voiceMode) {
        m_speechReady = false;
        if (m_speechProcess.state() != QProcess::NotRunning) {
#ifdef Q_OS_UNIX
            const auto process = m_speechProcess.processId();
            if (process > 0) ::kill(-process, SIGKILL);
#endif
            m_speechProcess.kill();
            m_speechProcess.waitForFinished(1000);
        }
        m_speechReady = false;
        m_speechBuffer.clear();
    }
    discardSpeechOutput(request);
    emit optionsChanged();
    emit speechStopped();
}
