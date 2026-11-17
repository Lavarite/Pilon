#include "codexruntime.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVersionNumber>

QString CodexRuntime::version(const QByteArray &output)
{
    static const QRegularExpression expression("(?:^|\\s)([0-9]+\\.[0-9]+\\.[0-9]+)(?:\\s|$)");
    return expression.match(QString::fromUtf8(output).trimmed()).captured(1);
}
QString CodexRuntime::executable(const QString &root)
{
    QFile marker(root + "/current");
    if (!marker.open(QIODevice::ReadOnly)) return {};
    const auto installed = version(marker.read(100));
    if (installed.isEmpty()) return {};
    const auto path = root + '/' + installed + "/node_modules/.bin/codex";
    return QFileInfo(path).isExecutable() ? path : QString{};
}
CodexRuntime::CodexRuntime(QObject *parent) : QObject(parent)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] { m_process.kill(); finish("Codex update timed out."); });
    connect(&m_timer, &QTimer::timeout, this, &CodexRuntime::check);
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_stage != Idle) finish(m_process.errorString());
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_timeout.stop();
        const auto output = m_process.readAllStandardOutput();
        if (m_stage == Idle) return;
        if (m_stage == Version) {
            fetchLatest(code == 0 && status == QProcess::NormalExit ? version(output) : QString{});
        } else if (code != 0 || status != QProcess::NormalExit) {
            finish(QString("Codex update failed: %1").arg(QString::fromUtf8(output).right(2000)));
        } else if (m_stage == Install) {
            m_stage = Verify; m_timeout.start(10000);
            m_process.start(m_candidate, {"--version"});
        } else if (version(output) != m_latest) {
            finish("Downloaded Codex did not report the expected version.");
        } else {
            QSaveFile marker(m_root + "/current");
            const auto bytes = m_latest.toUtf8();
            if (!marker.open(QIODevice::WriteOnly) || marker.write(bytes) != bytes.size() || !marker.commit()) {
                finish("Unable to activate the Codex update."); return;
            }
            qInfo().noquote() << "Pilon Codex updated to" << m_latest;
            finish(); emit updated();
        }
    });
}
CodexRuntime::~CodexRuntime()
{
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) { m_process.kill(); m_process.waitForFinished(1000); }
}
void CodexRuntime::start(const QString &root, const QString &fallback)
{
#ifdef Q_OS_ANDROID
    // Android executable updates are installed with the APK.
    Q_UNUSED(root);
    Q_UNUSED(fallback);
    return;
#endif
    if (m_timer.isActive() || qEnvironmentVariableIsSet("PILON_CODEX") || qEnvironmentVariableIsSet("PILON_EPHEMERAL_CHAT")) return;
    m_root = root; m_fallback = fallback;
    m_timer.start(4 * 60 * 60 * 1000);
    QTimer::singleShot(2500, this, [this, generation = m_generation] { if (generation == m_generation && m_timer.isActive()) check(); });
}
void CodexRuntime::stop()
{
    ++m_generation; m_timer.stop(); m_timeout.stop(); m_stage = Idle;
    for (auto *reply : m_network.findChildren<QNetworkReply *>()) reply->abort();
    m_process.kill(); m_process.waitForFinished(1000); m_lock.reset();
}
void CodexRuntime::check()
{
    if (m_stage != Idle || m_process.state() != QProcess::NotRunning) return;
    QFile checked(m_root + "/checked");
    if (checked.open(QIODevice::ReadOnly)) {
        const auto age = QDateTime::fromString(QString::fromUtf8(checked.readAll()), Qt::ISODate).secsTo(QDateTime::currentDateTimeUtc());
        if (age >= 0 && age < 86400) return;
    }
    if (!QDir().mkpath(m_root)) return;
    m_lock = std::make_unique<QLockFile>(m_root + "/update.lock");
    if (!m_lock->tryLock()) { m_lock.reset(); return; }
    const auto owned = executable(m_root);
    const auto path = owned.isEmpty() ? m_fallback : owned;
    m_stage = Version;
    if (path.isEmpty()) { fetchLatest({}); return; }
    m_timeout.start(10000); m_process.start(path, {"--version"});
}
void CodexRuntime::fetchLatest(const QString &installed)
{
    QNetworkRequest request(QUrl("https://registry.npmjs.org/@openai%2fcodex/latest"));
    request.setTransferTimeout(15000);
    auto *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, installed, generation = m_generation] {
        if (generation != m_generation) { reply->deleteLater(); return; }
        const auto bytes = reply->readAll(); const auto error = reply->error(); reply->deleteLater();
        if (error != QNetworkReply::NoError) { finish("Unable to check for Codex updates."); return; }
        m_latest = QJsonDocument::fromJson(bytes).object().value("version").toString();
        if (version(m_latest.toUtf8()) != m_latest || m_latest.isEmpty()) { finish("Invalid Codex release metadata."); return; }
        if (!installed.isEmpty() && QVersionNumber::fromString(m_latest) <= QVersionNumber::fromString(installed)) { finish(); return; }
        const auto npm = QStandardPaths::findExecutable("npm");
        if (npm.isEmpty()) { finish("npm is required to update Pilon's Codex runtime."); return; }
        const auto directory = m_root + '/' + m_latest;
        if (!QDir().mkpath(directory)) { finish("Unable to prepare the Codex runtime directory."); return; }
        m_candidate = directory + "/node_modules/.bin/codex";
        m_stage = Install; m_timeout.start(300000);
        m_process.start(npm, {"install", "--prefix", directory, "--no-audit", "--no-fund", "@openai/codex@" + m_latest});
    });
}
void CodexRuntime::finish(const QString &error)
{
    m_timeout.stop(); m_stage = Idle;
    m_lock.reset();
    if (!error.isEmpty()) { qWarning().noquote() << error; return; }
    QDir().mkpath(m_root);
    QSaveFile checked(m_root + "/checked");
    if (checked.open(QIODevice::WriteOnly)) { checked.write(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8()); checked.commit(); }
}
