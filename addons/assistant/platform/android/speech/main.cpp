#include <audiocpp.h>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using ProtocolReply = std::function<void(const QJsonObject &)>;

void require(bool value, const QString &message) {
    if (!value) throw std::runtime_error(message.toStdString());
}
void check(audiocpp_status status) {
    if (status == AUDIOCPP_ERR_OUT_OF_MEMORY) throw std::bad_alloc();
    if (status != AUDIOCPP_OK) throw std::runtime_error(audiocpp_last_error());
}
void publish(const QString &source, const QString &destination) {
    if (::rename(QFile::encodeName(source).constData(), QFile::encodeName(destination).constData()) != 0)
        throw std::runtime_error(std::strerror(errno));
}
QStringList segments(QString text) {
    QStringList result;
    while (!text.isEmpty()) {
        const auto points = text.toUcs4();
        if (points.size() <= 200) { result.append(text); break; }
        const auto prefix = QString::fromUcs4(reinterpret_cast<const char32_t *>(points.constData()), 200);
        int end = std::max({prefix.lastIndexOf(". "), prefix.lastIndexOf("! "), prefix.lastIndexOf("? "), prefix.lastIndexOf('\n')});
        if (end < 0) end = prefix.lastIndexOf(' ');
        end = end >= 0 ? end + 1 : prefix.size();
        result.append(text.left(end)); text.remove(0, end);
    }
    return result;
}
QByteArray ffmpeg(const QString &program, const QStringList &arguments) {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted()) throw std::runtime_error(process.errorString().toStdString());
    QByteArray output;
    while (!process.waitForFinished(100)) output += process.readAllStandardOutput();
    output += process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const auto error = QString::fromUtf8(process.readAllStandardError()).trimmed().right(4096);
        throw std::runtime_error(error.isEmpty() ? "FFmpeg stopped without producing speech audio." : error.toStdString());
    }
    return output;
}
struct SpeechRender {
    QString encoded;
    QString tempo;
    ~SpeechRender() {
        if (encoded.isEmpty()) return;
        QFile::remove(encoded);
        QFile::remove(encoded + ".partial.wav");
        QFile::remove(tempo);
    }
    void publishAtSpeed(const QString &program, const QString &output, int speed) const {
        if (speed == 5) { publish(encoded, output); return; }
        require(QFileInfo(program).isExecutable(), "FFmpeg is unavailable for changing speech speed.");
        ffmpeg(program, {"-nostdin", "-v", "error", "-y", "-i", encoded, "-af", QString("atempo=%1").arg(0.5 + speed / 10.0, 0, 'f', 1), "-c:a", "pcm_s16le", tempo});
        publish(tempo, output);
    }
};
std::vector<float> referenceAudio(const QString &program, const QString &path) {
    require(QFileInfo(path).isFile(), "The saved voice sample is missing. Preview and save a voice again.");
    const auto bytes = ffmpeg(program, {"-nostdin", "-v", "error", "-i", path, "-ac", "1", "-ar", "24000", "-f", "f32le", "pipe:1"});
    require(!bytes.isEmpty() && bytes.size() % 4 == 0, "The saved voice sample contains no audio.");
    std::vector<float> samples(bytes.size() / 4);
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto bits = qFromLittleEndian<quint32>(bytes.constData() + i * 4);
        std::memcpy(&samples[i], &bits, sizeof(float));
        require(std::isfinite(samples[i]), "The saved voice sample contains invalid audio.");
    }
    return samples;
}
class Wave {
    QString destination;
    QFile file;
    quint64 bytes = 0;
    int rate = 0, channels = 0;
    bool completed = false;
    ProtocolReply reply;
    QString tempoProgram;
    int speed;
    std::unique_ptr<QProcess> tempo;
    QByteArray tempoRemainder;

    void writePcm(const QByteArray &pcm) {
        require(quint64(pcm.size()) <= 0xffffffffULL - 36 - bytes, "The speech audio exceeds WAV's size limit.");
        if (file.write(pcm) != pcm.size()) throw std::runtime_error(file.errorString().toStdString());
        bytes += pcm.size();
        if (reply) {
            const qsizetype packet = 16384 / (2 * channels) * (2 * channels);
            for (qsizetype i = 0; i < pcm.size(); i += packet)
                reply({{"pcm", QString::fromLatin1(pcm.mid(i, packet).toBase64())},
                       {"rate", rate}, {"channels", channels}});
        }
    }
    void readTempo() {
        auto pcm = tempoRemainder + tempo->readAllStandardOutput();
        const auto remainder = pcm.size() % (2 * channels);
        tempoRemainder = pcm.last(remainder);
        pcm.chop(remainder);
        if (!pcm.isEmpty()) writePcm(pcm);
    }
    void checkTempo() {
        if (tempo->exitStatus() == QProcess::NormalExit && tempo->exitCode() == 0) return;
        const auto message = QString::fromUtf8(tempo->readAllStandardError()).trimmed().right(4096);
        throw std::runtime_error(message.isEmpty() ? "FFmpeg stopped without producing speech audio." : message.toStdString());
    }
public:
    explicit Wave(const QString &path, ProtocolReply stream = {}, const QString &program = {}, int tempoSpeed = 5)
        : destination(path), file(path + ".partial.wav"), reply(std::move(stream)), tempoProgram(program), speed(tempoSpeed) {
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) throw std::runtime_error(file.errorString().toStdString());
        if (file.write(QByteArray(44, '\0')) != 44) { file.remove(); throw std::runtime_error("Unable to save speech audio."); }
    }
    ~Wave() { if (!completed) file.remove(); }
    void append(const float *samples, size_t frames, int sampleRate, int channelCount) {
        require(frames && samples && sampleRate > 0 && channelCount > 0, "The speech engine returned no audio.");
        require(!rate || (rate == sampleRate && channels == channelCount), "Speech segments returned different audio formats.");
        rate = sampleRate; channels = channelCount;
        require(frames <= (0xffffffffULL - 36 - bytes) / (2 * channelCount), "The speech audio exceeds WAV's size limit.");
        QByteArray pcm(frames * channelCount * 2, Qt::Uninitialized);
        for (size_t i = 0; i < frames * channelCount; ++i) {
            require(std::isfinite(samples[i]), "The speech engine returned invalid audio.");
            const auto sample = qint16(std::clamp(std::lrint(std::clamp(samples[i], -1.0f, 1.0f) * 32768.0f), -32768L, 32767L));
            qToLittleEndian<qint16>(sample, pcm.data() + i * 2);
        }
        if (speed == 5) {
            writePcm(pcm);
            return;
        }
        if (!tempo) {
            require(QFileInfo(tempoProgram).isExecutable(), "FFmpeg is unavailable for changing speech speed.");
            tempo = std::make_unique<QProcess>();
            const auto multiplier = QString("atempo=%1").arg(0.5 + speed / 10.0, 0, 'f', 1);
            tempo->start(tempoProgram, {"-nostdin", "-v", "error", "-probesize", "32", "-analyzeduration", "0",
                "-f", "s16le", "-ar", QString::number(rate), "-ac", QString::number(channels), "-i", "pipe:0",
                "-af", multiplier, "-flush_packets", "1", "-f", "s16le", "pipe:1"});
            if (!tempo->waitForStarted()) throw std::runtime_error(tempo->errorString().toStdString());
        }
        require(tempo->write(pcm) == pcm.size(), "Unable to send speech audio to FFmpeg.");
        tempo->waitForBytesWritten(1);
        while (tempo->bytesToWrite() > 65536) {
            require(tempo->waitForBytesWritten(5000), "FFmpeg could not change speech speed.");
            readTempo();
        }
        if (tempo->state() != QProcess::NotRunning) tempo->waitForReadyRead(1);
        readTempo();
    }
    void finish() {
        if (tempo) {
            tempo->closeWriteChannel();
            while (tempo->state() != QProcess::NotRunning) {
                tempo->waitForFinished(100);
                readTempo();
            }
            checkTempo();
            readTempo();
            require(tempoRemainder.isEmpty(), "FFmpeg returned an incomplete audio frame.");
        }
        require(bytes && rate, "The speech engine returned no audio.");
        QByteArray header(44, '\0');
        std::memcpy(header.data(), "RIFF", 4); std::memcpy(header.data() + 8, "WAVEfmt ", 8); std::memcpy(header.data() + 36, "data", 4);
        qToLittleEndian<quint32>(bytes + 36, header.data() + 4); qToLittleEndian<quint32>(16, header.data() + 16);
        qToLittleEndian<quint16>(1, header.data() + 20); qToLittleEndian<quint16>(channels, header.data() + 22);
        qToLittleEndian<quint32>(rate, header.data() + 24); qToLittleEndian<quint32>(rate * channels * 2, header.data() + 28);
        qToLittleEndian<quint16>(channels * 2, header.data() + 32); qToLittleEndian<quint16>(16, header.data() + 34);
        qToLittleEndian<quint32>(bytes, header.data() + 40);
        if (!file.seek(0) || file.write(header) != header.size() || !file.flush()) throw std::runtime_error(file.errorString().toStdString());
        file.close(); publish(file.fileName(), destination); completed = true;
    }
};
struct Runtime {
    audiocpp_registry *registry = nullptr;
    audiocpp_model *model = nullptr;
    audiocpp_session *session = nullptr;
    QByteArray family;
    int threads = 0;
    bool streaming = false;
    ~Runtime() { audiocpp_session_free(session); audiocpp_model_free(model); audiocpp_registry_free(registry); }
    void load(const QString &path, int threads, const QByteArray &modelFamily) {
        require((audiocpp_abi_version() >> 16) == AUDIOCPP_ABI_VERSION_MAJOR, "The speech engine ABI is incompatible.");
        require(modelFamily == "breeze_tts" || modelFamily == "supertonic" || modelFamily == "gradium_phonon", "Unsupported speech model.");
        family = modelFamily;
        this->threads = threads;
        check(audiocpp_registry_create(nullptr, &registry));
        const audiocpp_model_config configuration{family.constData(), nullptr, nullptr, nullptr};
        const auto filename = QFile::encodeName(path);
        check(audiocpp_model_load(registry, filename.constData(), &configuration, nullptr, &model));
    }
    void run(audiocpp_request *request, Wave &output, bool stream = false) {
        // Breeze's C API completes each segment synchronously, so publish between segments.
        stream = stream && family != "breeze_tts" && audiocpp_model_supports(model, "tts", "streaming");
        if (!session || streaming != stream) {
            audiocpp_session_free(session);
            session = nullptr;
            const audiocpp_backend_config backend{"cpu", 0, threads};
            check(audiocpp_session_create(model, "tts", stream ? "streaming" : "offline", &backend, nullptr, &session));
            streaming = stream;
        }
        audiocpp_result *raw = nullptr;
        if (stream) {
            check(audiocpp_stream_start(session, request));
            for (;;) {
                audiocpp_event *event = nullptr;
                const auto status = audiocpp_stream_next_event(session, &event);
                const std::unique_ptr<audiocpp_event, decltype(&audiocpp_event_free)> release(event, audiocpp_event_free);
                check(status);
                if (!event) break;
                const auto *result = audiocpp_event_as_result(event);
                for (size_t i = 0; i < audiocpp_result_named_audio_count(result); ++i) {
                    const float *audio = nullptr;
                    size_t frames = 0;
                    int rate = 0, channels = 0;
                    check(audiocpp_result_named_audio(result, i, nullptr, &audio, &frames, &rate, &channels));
                    if (frames) output.append(audio, frames, rate, channels);
                }
            }
            const auto status = audiocpp_stream_finish(session, &raw);
            const std::unique_ptr<audiocpp_result, decltype(&audiocpp_result_free)> release(raw, audiocpp_result_free);
            check(status);
            return;
        }
        const auto status = audiocpp_session_run(session, request, &raw);
        std::unique_ptr<audiocpp_result, decltype(&audiocpp_result_free)> result(raw, audiocpp_result_free);
        check(status);
        const float *audio = nullptr; size_t frames = 0; int rate = 0, channels = 0;
        check(audiocpp_result_audio(result.get(), &audio, &frames, &rate, &channels));
        output.append(audio, frames, rate, channels);
    }
    void synthesizeText(const QString &text, const QJsonObject &settings, Wave &output, bool stream) {
        std::unique_ptr<audiocpp_request, decltype(&audiocpp_request_free)> request(audiocpp_request_create(), audiocpp_request_free);
        require(bool(request), "Unable to create the speech request.");
        const auto encoded = text.toUtf8();
        const auto language = settings.value("language").toString().toUtf8();
        check(audiocpp_request_set_text(request.get(), encoded.constData(), family == "supertonic" ? language.constData() : nullptr));
        if (family == "supertonic") {
            const auto voice = settings.value("preset").toString().toUtf8();
            check(audiocpp_request_set_voice_id(request.get(), voice.constData()));
        } else {
            const auto voice = settings.value("voiceEmbedding").toString();
            const auto temperature = settings.value("temperature").toDouble(0.3);
            require(std::isfinite(temperature) && temperature > 0 && temperature <= 1, "Choose a voice temperature above zero and at most 1.");
            check(audiocpp_request_set_option(request.get(), "temperature", QByteArray::number(temperature).constData()));
            if (!voice.isEmpty()) {
                const auto file = QFileInfo(voice);
                require(file.isAbsolute() && file.isFile() && file.isReadable()
                    && file.suffix().compare("safetensors", Qt::CaseInsensitive) == 0, "Choose a readable Phonon voice embedding file.");
                check(audiocpp_request_set_option(request.get(), "voice_embedding_file", QFile::encodeName(voice).constData()));
            }
        }
        run(request.get(), output, stream);
    }
    void synthesize(const QString &text, const QString &instruction, Wave &output,
        const QJsonObject &reference, const std::vector<float> &samples) {
        std::unique_ptr<audiocpp_request, decltype(&audiocpp_request_free)> request(audiocpp_request_create(), audiocpp_request_free);
        require(bool(request), "Unable to create the Breeze request.");
        const auto encoded = text.toUtf8(); check(audiocpp_request_set_text(request.get(), encoded.constData(), nullptr));
        const auto option = [&](const char *name, const QByteArray &value) { check(audiocpp_request_set_option(request.get(), name, value.constData())); };
        option("instruct", instruction.toUtf8()); option("seed", "42"); option("max_tokens", "1500");
        option("temperature", "0.9"); option("depth_temperature", "0.9"); option("top_k", "50"); option("top_p", "1.0");
        option("guidance_scale", reference.isEmpty() ? "4.0" : "1.0");
        if (!reference.isEmpty()) {
            option("reference_text", reference.value("text").toString().toUtf8());
            check(audiocpp_request_set_voice_audio(request.get(), samples.data(), samples.size(), 24000, 1));
        }
        run(request.get(), output);
    }
};
QJsonObject process(const QJsonObject &request, Runtime &runtime, const QString &program, const ProtocolReply &reply) {
    const int speed = request.value("speed").toInt(5);
    require(speed >= 1 && speed <= 10, "Voice speed must be between 1 and 10.");
    const auto output = request.value("output").toString();
    require(QDir::isAbsolutePath(output), "Choose an absolute speech output path.");
    const QString rendered = output + ".render.wav", tempo = output + ".tempo.wav";
    const auto pcmReply = request.value("stream").toBool() ? reply : ProtocolReply{};
    if (runtime.family != "breeze_tts") {
        const auto text = request.value("text").toString();
        require(!text.isEmpty(), "Provide text to speak.");
        // Supertonic's internal speed parameter can truncate the utterance.
        const bool adjustSpeed = runtime.family == "supertonic" && speed != 5;
        Wave speech(output, pcmReply, program, adjustSpeed ? speed : 5);
        runtime.synthesizeText(text, request, speech, bool(pcmReply));
        speech.finish();
        return {{"output", output}};
    }
    if (!request.value("preview").toBool()) require(!request.value("text").toString().isEmpty(), "Provide text to speak.");
    auto reference = request.value("reference").toObject();
    const bool createReference = !request.contains("reference");
    bool keepReference = false;
    const auto referenceOutput = request.value("reference_output").toString();
    struct ReferenceCleanup {
        QString path; bool created; bool &keep;
        ~ReferenceCleanup() { if (created && !keep) { QFile::remove(path); QFile::remove(path + ".partial.wav"); } }
    } referenceCleanup{referenceOutput, false, keepReference};
    if (createReference) {
        const auto text = request.value("reference_text").toString();
        const auto points = request.value("instruction").toString().toUcs4();
        const auto instruction = QString::fromUcs4(reinterpret_cast<const char32_t *>(points.constData()), std::min(qsizetype(2000), points.size()));
        require(QDir::isAbsolutePath(referenceOutput) && !text.isEmpty() && !instruction.isEmpty(), "Provide a voice instruction and reference transcript.");
        require(referenceOutput != output && referenceOutput != rendered && referenceOutput != tempo, "Speech output must not replace the voice sample.");
        require(!QFile::exists(referenceOutput), "Choose a new path for the voice sample.");
        Wave sample(referenceOutput); runtime.synthesize(text, instruction, sample, {}, {}); sample.finish();
        referenceCleanup.created = true;
        reference = {{"path", referenceOutput}, {"text", text}};
    }
    const auto path = reference.value("path").toString(), transcript = reference.value("text").toString();
    require(!transcript.isEmpty() && QDir::isAbsolutePath(path) && QFileInfo(path).isFile(), "The saved voice sample is missing. Preview and save a voice again.");
    require(path != output && path != rendered && path != rendered + ".partial.wav" && path != tempo, "Speech output must not replace the voice sample.");
    const SpeechRender cleanup{rendered, tempo};
    if (request.value("preview").toBool()) {
        require(QFile::copy(path, rendered), "Unable to copy the voice sample.");
        cleanup.publishAtSpeed(program, output, speed);
    } else {
        const auto text = request.value("text").toString();
        const auto samples = referenceAudio(program, path);
        Wave speech(rendered, pcmReply, program, speed);
        for (const auto &segment : segments(text)) runtime.synthesize(segment, "Speak clearly and naturally.", speech, reference, samples);
        speech.finish();
        publish(rendered, output);
    }
    keepReference = true;
    return {{"output", output}, {"reference", reference}};
}
void selfTest(const QString &program, const QString &directory) {
    QTemporaryDir temporary(directory + "/breeze-test-XXXXXX");
    require(temporary.isValid(), "Cannot create the Breeze test directory.");
    const auto text = QString::fromUtf8("A complete sentence. café 😀\n").repeated(200) + "Final conclusion.";
    const auto split = segments(text);
    require(split.join("") == text && split.size() > 1, "Long speech text was dropped.");
    for (const auto &part : split) require(part.toUcs4().size() <= 200, "A speech segment exceeds the model limit.");
    const auto unbroken = QString::fromUtf8("文字😀").repeated(500);
    require(segments(unbroken).join("") == unbroken, "Unbroken Unicode text was dropped.");
    std::vector<float> tone(48000);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.3f * std::sin(2 * 3.141592653589793 * 440 * i / 24000);
    for (const int speed : {1, 5, 10}) {
        const auto output = temporary.filePath(QString("speed-%1.wav").arg(speed));
        QByteArray streamed;
        Wave writer(output, [&](const QJsonObject &packet) {
            require(!QFileInfo::exists(output) && packet.value("rate") == 24000 && packet.value("channels") == 1,
                "Speech must stream before publishing its WAV.");
            streamed += QByteArray::fromBase64(packet.value("pcm").toString().toLatin1());
        }, program, speed);
        for (size_t i = 0; i < tone.size(); i += 1920)
            writer.append(tone.data() + i, 1920, 24000, 1);
        writer.finish();
        QFile wav(output);
        require(wav.open(QIODevice::ReadOnly) && wav.seek(44) && wav.readAll() == streamed,
            "The published WAV differs from the speech stream.");
        const auto samples = referenceAudio(program, output);
        const double duration = samples.size() / 24000.0;
        require(std::abs(duration - 2 / (0.5 + speed / 10.0)) < 0.06, "Speech speed did not change duration correctly.");
        size_t crossings = 0;
        for (size_t i = 1; i < samples.size(); ++i) if (samples[i - 1] < 0 && samples[i] >= 0) ++crossings;
        require(std::abs(crossings / duration - 440) < 5, "Speech speed changed pitch.");
    }
}
}
int main(int argc, char **argv) {
    ::umask(0077);
    const int descriptor = ::dup(STDOUT_FILENO);
    if (descriptor < 0 || ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0 || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return 2;
    FILE *protocol = ::fdopen(descriptor, "w");
    if (!protocol) return 2;
    const auto reply = [&](const QJsonObject &value) { const auto json = QJsonDocument(value).toJson(QJsonDocument::Compact); fwrite(json.data(), 1, json.size(), protocol); fputc('\n', protocol); fflush(protocol); };
    try {
        require(argc >= 3, "Provide the speech model and FFmpeg paths.");
        if (QString::fromUtf8(argv[1]) == "--self-test") {
            require(argc == 4, "Provide FFmpeg and a test folder."); selfTest(QString::fromUtf8(argv[2]), QString::fromUtf8(argv[3]));
            reply({{"self_test", true}, {"abi", int(audiocpp_abi_version())}, {"tasks", int(audiocpp_task_count())}}); return 0;
        }
        const auto program = QString::fromUtf8(argv[2]);
        const auto family = argc > 4 ? QByteArray(argv[4]) : QByteArray("breeze_tts");
        if (family == "breeze_tts") require(QFileInfo(program).isExecutable(), "FFmpeg is unavailable.");
        Runtime runtime;
        const int threads = argc > 3 ? QString::fromUtf8(argv[3]).toInt() : std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        require(threads > 0, "Choose a positive speech thread count.");
        runtime.load(QString::fromUtf8(argv[1]), threads, family);
        reply({{"ready", true}});
        for (std::string line; std::getline(std::cin, line);) {
            try {
                QJsonParseError error;
                const auto input = QJsonDocument::fromJson(QByteArray::fromStdString(line), &error);
                require(error.error == QJsonParseError::NoError && input.isObject(), "Invalid speech request.");
                reply(process(input.object(), runtime, program, reply));
            } catch (const std::bad_alloc &) { reply({{"error", "Not enough memory to generate speech."}}); return 1; }
            catch (const std::exception &error) { reply({{"error", QString::fromUtf8(error.what())}}); }
        }
    } catch (const std::bad_alloc &) { reply({{"error", "Not enough memory to load speech output."}}); return 1; }
    catch (const std::exception &error) { reply({{"error", QString::fromUtf8(error.what())}}); return 1; }
}
