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
    connect(&m_timeout, &QTimer::timeout, this, [this] { m_process.kill(); finish(tr("Codex update timed out.")); });
    connect(&m_timer, &QTimer::timeout, this, [this] { check(); });
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
            qWarning().noquote() << QString::fromUtf8(output).right(2000);
            finish(tr("Unable to install the Codex update."));
        } else if (m_stage == Install) {
            m_stage = Verify; m_timeout.start(10000);
            m_process.start(m_candidate, {"--version"});
        } else if (version(output) != m_latest) {
            finish(tr("Downloaded Codex did not report the expected version."));
        } else {
            QSaveFile marker(m_root + "/current");
            const auto bytes = m_latest.toUtf8();
            if (!marker.open(QIODevice::WriteOnly) || marker.write(bytes) != bytes.size() || !marker.commit()) {
                finish(tr("Unable to activate the Codex update.")); return;
            }
            m_installed = m_latest;
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
bool CodexRuntime::supported() const
{
#ifdef Q_OS_ANDROID
    // Android executable updates are installed with the APK.
    return false;
#else
    return !qEnvironmentVariableIsSet("PILON_CODEX") && !qEnvironmentVariableIsSet("PILON_EPHEMERAL_CHAT");
#endif
}
void CodexRuntime::start(const QString &root, const QString &fallback)
{
    if (m_timer.isActive() || !supported()) return;
    m_root = root; m_fallback = fallback;
    const auto current = executable(root);
    m_installed = current.isEmpty() ? QString{} : QDir(root).relativeFilePath(current).section('/', 0, 0);
    m_timer.start(4 * 60 * 60 * 1000);
    QTimer::singleShot(2500, this, [this, generation = m_generation] { if (generation == m_generation && m_timer.isActive()) check(); });
    emit checkChanged();
}
void CodexRuntime::stop()
{
    ++m_generation; m_timer.stop(); m_timeout.stop(); m_stage = Idle; m_manual = false;
    for (auto *reply : m_network.findChildren<QNetworkReply *>()) reply->abort();
    m_process.kill(); m_process.waitForFinished(1000); m_lock.reset();
    m_status.clear();
    emit checkChanged();
}
void CodexRuntime::check(bool manual)
{
    if (!m_timer.isActive() || m_stage != Idle || m_process.state() != QProcess::NotRunning) return;
    QFile checked(m_root + "/checked");
    if (!manual && checked.open(QIODevice::ReadOnly)) {
        const auto age = QDateTime::fromString(QString::fromUtf8(checked.readAll()), Qt::ISODate).secsTo(QDateTime::currentDateTimeUtc());
        if (age >= 0 && age < 86400) return;
    }
    m_manual = manual;
    if (!QDir().mkpath(m_root)) { finish(tr("Unable to prepare the Codex runtime directory.")); return; }
    m_lock = std::make_unique<QLockFile>(m_root + "/update.lock");
    if (!m_lock->tryLock()) { finish(tr("Codex is already being checked by another Pilon instance.")); return; }
    const auto owned = executable(m_root);
    const auto path = owned.isEmpty() ? m_fallback : owned;
    m_stage = Version;
    if (manual) m_status = tr("Checking for Codex updates…");
    emit checkChanged();
    if (path.isEmpty()) { fetchLatest({}); return; }
    m_timeout.start(10000); m_process.start(path, {"--version"});
}
void CodexRuntime::fetchLatest(const QString &installed)
{
    if (!installed.isEmpty()) m_installed = installed;
    QNetworkRequest request(QUrl("https://registry.npmjs.org/@openai%2fcodex/latest"));
    request.setTransferTimeout(15000);
    auto *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, installed, generation = m_generation] {
        if (generation != m_generation) { reply->deleteLater(); return; }
        const auto bytes = reply->readAll(); const auto error = reply->error(); reply->deleteLater();
        if (error != QNetworkReply::NoError) { finish(tr("Unable to check for Codex updates.")); return; }
        m_latest = QJsonDocument::fromJson(bytes).object().value("version").toString();
        if (version(m_latest.toUtf8()) != m_latest || m_latest.isEmpty()) { finish(tr("Invalid Codex release metadata.")); return; }
        if (!installed.isEmpty() && QVersionNumber::fromString(m_latest) <= QVersionNumber::fromString(installed)) { finish(); return; }
        const auto npm = QStandardPaths::findExecutable("npm");
        if (npm.isEmpty()) { finish(tr("Install npm to update Codex.")); return; }
        const auto directory = m_root + '/' + m_latest;
        if (!QDir().mkpath(directory)) { finish(tr("Unable to prepare the Codex runtime directory.")); return; }
        m_candidate = directory + "/node_modules/.bin/codex";
        m_stage = Install; m_timeout.start(300000);
        if (m_manual) m_status = tr("Installing Codex %1…").arg(m_latest);
        emit checkChanged();
        m_process.start(npm, {"install", "--prefix", directory, "--no-audit", "--no-fund", "@openai/codex@" + m_latest});
    });
}
void CodexRuntime::finish(const QString &error)
{
    m_timeout.stop(); m_stage = Idle;
    m_lock.reset();
    if (m_manual) m_status = error.isEmpty() ? tr("Codex is up to date.") : error;
    m_manual = false;
    if (!error.isEmpty()) {
        qWarning().noquote() << error;
    } else {
        QDir().mkpath(m_root);
        QSaveFile checked(m_root + "/checked");
        if (checked.open(QIODevice::WriteOnly)) {
            checked.write(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8());
            checked.commit();
        }
    }
    emit checkChanged();
}
