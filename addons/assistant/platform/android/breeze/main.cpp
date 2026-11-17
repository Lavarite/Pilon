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
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
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
public:
    explicit Wave(const QString &path) : destination(path), file(path + ".partial.wav") {
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) throw std::runtime_error(file.errorString().toStdString());
        if (file.write(QByteArray(44, '\0')) != 44) { file.remove(); throw std::runtime_error("Unable to save speech audio."); }
    }
    ~Wave() { if (!completed) file.remove(); }
    void append(const float *samples, size_t frames, int sampleRate, int channelCount) {
        require(frames && samples && sampleRate > 0 && channelCount > 0, "Breeze returned no audio.");
        require(!rate || (rate == sampleRate && channels == channelCount), "Speech segments returned different audio formats.");
        rate = sampleRate; channels = channelCount;
        require(frames <= (0xffffffffULL - 36 - bytes) / (2 * channelCount), "The speech audio exceeds WAV's size limit.");
        QByteArray pcm(frames * channelCount * 2, Qt::Uninitialized);
        for (size_t i = 0; i < frames * channelCount; ++i) {
            require(std::isfinite(samples[i]), "Breeze returned invalid audio.");
            const auto sample = qint16(std::clamp(std::lrint(std::clamp(samples[i], -1.0f, 1.0f) * 32768.0f), -32768L, 32767L));
            qToLittleEndian<qint16>(sample, pcm.data() + i * 2);
        }
        if (file.write(pcm) != pcm.size()) throw std::runtime_error(file.errorString().toStdString());
        bytes += pcm.size();
    }
    void finish() {
        require(bytes && rate, "Breeze returned no audio.");
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
    ~Runtime() { audiocpp_session_free(session); audiocpp_model_free(model); audiocpp_registry_free(registry); }
    void load(const QString &path, int threads) {
        require((audiocpp_abi_version() >> 16) == AUDIOCPP_ABI_VERSION_MAJOR, "The Breeze engine ABI is incompatible.");
        check(audiocpp_registry_create(nullptr, &registry));
        const audiocpp_model_config configuration{"breeze_tts", nullptr, nullptr, nullptr};
        const auto filename = QFile::encodeName(path);
        check(audiocpp_model_load(registry, filename.constData(), &configuration, nullptr, &model));
        const audiocpp_backend_config backend{"cpu", 0, threads};
        check(audiocpp_session_create(model, "tts", "offline", &backend, nullptr, &session));
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
        audiocpp_result *raw = nullptr;
        const auto status = audiocpp_session_run(session, request.get(), &raw);
        std::unique_ptr<audiocpp_result, decltype(&audiocpp_result_free)> result(raw, audiocpp_result_free);
        check(status);
        const float *audio = nullptr; size_t frames = 0; int rate = 0, channels = 0;
        check(audiocpp_result_audio(result.get(), &audio, &frames, &rate, &channels));
        output.append(audio, frames, rate, channels);
    }
};
QJsonObject process(const QJsonObject &request, Runtime &runtime, const QString &program) {
    const int speed = request.value("speed").toInt(5);
    require(speed >= 1 && speed <= 10, "Voice speed must be between 1 and 10.");
    const auto output = request.value("output").toString();
    require(QDir::isAbsolutePath(output), "Choose an absolute speech output path.");
    if (!request.value("preview").toBool()) require(!request.value("text").toString().isEmpty(), "Provide text to speak.");
    const QString rendered = output + ".render.wav", tempo = output + ".tempo.wav";
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
    struct Cleanup { QStringList files; ~Cleanup() { for (const auto &file : files) QFile::remove(file); } } cleanup{{rendered, rendered + ".partial.wav", tempo}};
    if (request.value("preview").toBool()) {
        require(QFile::copy(path, rendered), "Unable to copy the voice sample.");
    } else {
        const auto text = request.value("text").toString();
        const auto samples = referenceAudio(program, path);
        Wave speech(rendered);
        for (const auto &segment : segments(text)) runtime.synthesize(segment, "Speak clearly and naturally.", speech, reference, samples);
        speech.finish();
    }
    if (speed != 5) {
        ffmpeg(program, {"-nostdin", "-v", "error", "-y", "-i", rendered, "-af", QString("atempo=%1").arg(0.5 + speed / 10.0, 0, 'f', 1), "-c:a", "pcm_s16le", tempo});
        publish(tempo, output);
    } else publish(rendered, output);
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
    const auto source = temporary.filePath("tone.wav");
    { Wave writer(source); writer.append(tone.data(), tone.size(), 24000, 1); writer.finish(); }
    for (const int speed : {1, 5, 10}) {
        const auto output = temporary.filePath(QString("speed-%1.wav").arg(speed));
        if (speed == 5) require(QFile::copy(source, output), "Cannot copy the test tone.");
        else ffmpeg(program, {"-nostdin", "-v", "error", "-y", "-i", source, "-af", QString("atempo=%1").arg(0.5 + speed / 10.0, 0, 'f', 1), "-c:a", "pcm_s16le", output});
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
        require(argc >= 3, "Provide the Breeze model and FFmpeg paths.");
        if (QString::fromUtf8(argv[1]) == "--self-test") {
            require(argc == 4, "Provide FFmpeg and a test folder."); selfTest(QString::fromUtf8(argv[2]), QString::fromUtf8(argv[3]));
            reply({{"self_test", true}, {"abi", int(audiocpp_abi_version())}, {"tasks", int(audiocpp_task_count())}}); return 0;
        }
        const auto program = QString::fromUtf8(argv[2]);
        require(QFileInfo(program).isExecutable(), "FFmpeg is unavailable.");
        Runtime runtime;
        const int threads = argc > 3 ? QString::fromUtf8(argv[3]).toInt() : std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        require(threads > 0, "Choose a positive speech thread count.");
        runtime.load(QString::fromUtf8(argv[1]), threads); reply({{"ready", true}});
        for (std::string line; std::getline(std::cin, line);) {
            try {
                QJsonParseError error;
                const auto input = QJsonDocument::fromJson(QByteArray::fromStdString(line), &error);
                require(error.error == QJsonParseError::NoError && input.isObject(), "Invalid speech request.");
                reply(process(input.object(), runtime, program));
            } catch (const std::bad_alloc &) { reply({{"error", "Not enough memory to generate speech."}}); return 1; }
            catch (const std::exception &error) { reply({{"error", QString::fromUtf8(error.what())}}); }
        }
    } catch (const std::bad_alloc &) { reply({{"error", "Not enough memory to load speech output."}}); return 1; }
    catch (const std::exception &error) { reply({{"error", QString::fromUtf8(error.what())}}); return 1; }
}
