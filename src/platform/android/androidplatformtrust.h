#pragma once
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSslCertificate>
#include <QSslConfiguration>

namespace AndroidPlatformTrust {
inline QString exportCertificates(QProcessEnvironment &environment, const QString &path,
                                  const QList<QSslCertificate> &certificates)
{
    QByteArray pem;
    for (const auto &certificate : certificates) {
        const auto block = certificate.toPem();
        if (pem.size() + block.size() > 16 * 1024 * 1024)
            return QObject::tr("The platform certificate store is too large.");
        pem += block;
    }
    if (pem.isEmpty()) return QObject::tr("The platform certificate store is unavailable.");
    QSaveFile file(path);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !file.open(QIODevice::WriteOnly)
        || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        || file.write(pem) != pem.size() || !file.commit())
        return QObject::tr("The platform certificate bundle could not be saved.");
    environment.insert("SSL_CERT_FILE", file.fileName());
    return {};
}

inline QString prepareEnvironment(QProcessEnvironment &environment, const QString &path,
                                  const QList<QSslCertificate> &certificates)
{
    const auto codexCertificate = environment.value("CODEX_CA_CERTIFICATE");
    const auto sslCertificate = environment.value("SSL_CERT_FILE");
    const auto name = codexCertificate.isEmpty() ? "SSL_CERT_FILE" : "CODEX_CA_CERTIFICATE";
    const auto selected = codexCertificate.isEmpty() ? sslCertificate : codexCertificate;
    if (!selected.isEmpty() && selected != path) {
        QFile file(selected);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024
            || QSslCertificate::fromData(file.readAll(), QSsl::Pem).isEmpty())
            return QObject::tr("The custom certificate bundle selected by %1 is unavailable or invalid.").arg(QString::fromLatin1(name));
    }
    if (!sslCertificate.isEmpty() && sslCertificate != path) return {};
    return exportCertificates(environment, path, certificates);
}

#ifdef Q_OS_ANDROID
inline QMutex certificateMutex;
inline QString exportCertificates(QProcessEnvironment &environment)
{
    QMutexLocker lock(&certificateMutex);
    return exportCertificates(environment, QDir::homePath() + "/.cache/pilon-git/platform-ca.pem",
                              QSslConfiguration::systemCaCertificates());
}
inline QString prepareEnvironment(QProcessEnvironment &environment)
{
    QMutexLocker lock(&certificateMutex);
    // Qt obtains Android's trusted certificates through its platform certificate API.
    return prepareEnvironment(environment, QDir::homePath() + "/.cache/pilon-git/platform-ca.pem",
                              QSslConfiguration::systemCaCertificates());
}
#endif
}
