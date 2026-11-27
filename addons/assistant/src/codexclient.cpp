#include "codexclient.h"
#include <QJsonDocument>
#include <utility>
#ifdef Q_OS_ANDROID
#include "../../../src/platform/android/androidplatformtrust.h"
#include "../../../src/core/workspace/pilonworkspace.h"
#endif

CodexClient::CodexClient(QObject *parent) : QObject(parent)
{
    m_startTimer.setSingleShot(true);
    m_startTimer.setInterval(30000);
    connect(&m_startTimer, &QTimer::timeout, this, [this] {
        shutdown();
        emit failed(tr("Codex app-server did not initialize."));
    });
    connect(&m_process, &QProcess::started, this, [this] {
        request("initialize", {{"clientInfo", QJsonObject{{"name", "pilon"}, {"title", "Pilon"}, {"version", "1.0"}}},
            {"capabilities", QJsonObject{{"experimentalApi", true}}}}, [this](QJsonObject, QString error) {
            m_startTimer.stop();
            if (m_stopping || !running())
                return;
            if (!error.isEmpty()) { shutdown(); emit failed(error); return; }
            if (!write({{"method", "initialized"}})) {
                shutdown(); emit failed(tr("Could not initialize the Codex app-server.")); return;
            }
            m_ready = true;
            emit initialized();
        });
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &CodexClient::consume);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        m_log = (m_log + m_process.readAllStandardError()).right(8192);
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && !m_stopping) {
            m_startTimer.stop();
            const auto message = tr("Unable to start Codex: %1").arg(m_process.errorString());
            failPending(message);
            emit failed(message);
        }
    });
    connect(&m_process, &QProcess::finished, this, [this] {
        m_ready = false;
        m_startTimer.stop();
        if (!m_stopping) {
            const auto message = tr("Codex app-server stopped. %1").arg(QString::fromUtf8(m_log).trimmed()).trimmed();
            failPending(message);
            emit failed(message);
        }
    });
}

CodexClient::~CodexClient() { m_requests.clear(); shutdown(); }

void CodexClient::start(const QString &executable, const QStringList &arguments, const QProcessEnvironment &environment)
{
    if (running()) return;
    m_stopping = false;
    m_buffer.clear(); m_log.clear();
    auto processEnvironment = environment;
#ifdef Q_OS_ANDROID
    const auto dnsHelper = PilonWorkspace::packagedExecutable("dns");
    if (dnsHelper.isEmpty()) { emit failed(tr("The packaged network resolver is unavailable.")); return; }
    processEnvironment.insert("PILON_DNS_HELPER", dnsHelper);
    const auto trustError = AndroidPlatformTrust::prepareEnvironment(processEnvironment);
    if (!trustError.isEmpty()) { emit failed(trustError); return; }
#endif
    m_process.setProcessEnvironment(processEnvironment);
    m_startTimer.start();
    m_process.start(executable, arguments);
}

void CodexClient::shutdown()
{
    m_stopping = true; m_ready = false;
    m_startTimer.stop();
    if (running()) {
        m_process.closeWriteChannel();
        m_process.terminate();
        if (!m_process.waitForFinished(1000)) { m_process.kill(); m_process.waitForFinished(1000); }
    }
    failPending(tr("The Codex request was cancelled."));
}

bool CodexClient::write(const QJsonObject &message)
{
    if (m_stopping || m_process.state() != QProcess::Running)
        return false;
    const auto data = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    return m_process.write(data) == data.size();
}

void CodexClient::failPending(const QString &error)
{
    auto pending = std::exchange(m_requests, {});
    for (auto &reply : pending)
        reply({}, error);
}

void CodexClient::request(const QString &method, const QJsonObject &params, Reply reply)
{
    const auto id = ++m_nextId;
    if (!write({{"id", id}, {"method", method}, {"params", params}})) {
        if (reply)
            reply({}, tr("Could not send a request to the Codex app-server."));
        return;
    }
    if (!reply)
        return;
    m_requests.insert(id, std::move(reply));
    if (method != "initialize") {
        QTimer::singleShot(30000, this, [this, id] {
            auto pending = m_requests.take(id);
            if (pending)
                pending({}, tr("The Codex app-server did not respond."));
        });
    }
}

void CodexClient::respond(const QJsonValue &id, const QJsonObject &result) { write({{"id", id}, {"result", result}}); }
void CodexClient::reject(const QJsonValue &id, const QString &message)
{
    write({{"id", id}, {"error", QJsonObject{{"code", -32601}, {"message", message}}}});
}

void CodexClient::consume()
{
    m_buffer += m_process.readAllStandardOutput();
    for (;;) {
        const auto end = m_buffer.indexOf('\n');
        constexpr qsizetype maximumMessageSize = 16 * 1024 * 1024;
        if ((end < 0 ? m_buffer.size() : end) > maximumMessageSize) {
            m_buffer.clear();
            shutdown(); emit failed(tr("The Codex app-server returned an oversized message.")); return;
        }
        if (end < 0)
            return;
        const auto line = m_buffer.first(end); m_buffer.remove(0, end + 1);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            shutdown(); emit failed(tr("Codex app-server returned invalid JSON.")); return;
        }
        const auto message = document.object();
        const auto method = message.value("method").toString();
        if (!method.isEmpty()) {
            if (message.contains("id")) emit serverRequest(message.value("id"), method, message.value("params").toObject());
            else emit notification(method, message.value("params").toObject());
        } else if (m_requests.contains(message.value("id").toInteger())) {
            auto reply = m_requests.take(message.value("id").toInteger());
            if (reply) reply(message.value("result").toObject(), message.value("error").toObject().value("message").toString());
        }
    }
}
