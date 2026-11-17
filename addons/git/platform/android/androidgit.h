#pragma once
#include <QtGlobal>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStringList>

namespace AndroidGit {
inline QString repositoryRoot(const QString &selectedDirectory) {
    if (!QDir::isAbsolutePath(selectedDirectory) || selectedDirectory.contains(QChar::Null)
        || !QFileInfo(selectedDirectory).isDir()) return {};
    auto directory = QFileInfo(selectedDirectory).canonicalFilePath();
    while (!directory.isEmpty()) {
        const QFileInfo metadata(directory + "/.git");
        if (metadata.exists() || metadata.isSymLink())
            return directory.endsWith("/*") ? QString{} : directory;
        const auto parent = QFileInfo(directory).absolutePath();
        if (parent == directory) break;
        directory = parent;
    }
    return {};
}
inline void prepareRepositoryEnvironment(QProcessEnvironment &environment, const QString &selectedDirectory) {
    auto root = repositoryRoot(selectedDirectory);
    if (root.isEmpty()) return;
    root.replace('\'', "'\\''");
    root.replace('!', "'\\!'");
    // Match Git's command-scope quoting so provider child processes inherit only this checkout's exception.
    auto parameters = environment.value("GIT_CONFIG_PARAMETERS");
    if (!parameters.isEmpty()) parameters += ' ';
    environment.insert("GIT_CONFIG_PARAMETERS", parameters
        + "'safe.directory'='' 'safe.directory'='" + root + '\'');
}
}

#ifdef Q_OS_ANDROID
#include "androidplatformtrust.h"
#include "../../../../src/core/workspace/pilonworkspace.h"
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QProcessEnvironment>
#include <QMutex>
#include <QMutexLocker>

namespace AndroidGit {
inline QString prepareEnvironment(QProcessEnvironment &environment, bool certificates = true) {
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    const auto executable = PilonWorkspace::packagedExecutable("git");
    if (executable.isEmpty()) return QObject::tr("The packaged Git runtime is unavailable.");
    const auto directory = QDir::homePath() + "/.cache/pilon-git";
    if (!QDir().mkpath(directory + "/templates") || !QDir().mkpath(directory + "/bin"))
        return QObject::tr("The private Git runtime folder could not be created.");
    for (const auto *name : {"git", "ssh", "gh", "glab"}) {
        const auto target = PilonWorkspace::packagedExecutable(name);
        if (target.isEmpty()) continue;
        const auto link = directory + "/bin/" + name;
        const QFileInfo existing(link);
        if (existing.isSymLink() && existing.symLinkTarget() == target) continue;
        if (existing.exists() || existing.isSymLink()) {
            if (!existing.isSymLink() || !QFile::remove(link))
                return QObject::tr("The private Git runtime link could not be updated.");
        }
        if (!QFile::link(target, link)) return QObject::tr("The private Git runtime link could not be created.");
    }
    environment.insert("PATH", directory + "/bin:" + environment.value("PATH"));
    environment.insert("GIT_EXEC_PATH", QFileInfo(executable).absolutePath());
    environment.insert("GIT_TEMPLATE_DIR", directory + "/templates");
    environment.insert("GIT_SSH", PilonWorkspace::packagedExecutable("ssh"));
    environment.insert("GIT_SSH_VARIANT", "ssh");
    environment.insert("GIT_CONFIG_NOSYSTEM", "1");
    if (!certificates) return {};
    const auto trustError = AndroidPlatformTrust::exportCertificates(environment);
    if (!trustError.isEmpty()) return trustError;
    environment.insert("GIT_SSL_CAINFO", environment.value("SSL_CERT_FILE"));
    environment.remove("GIT_SSL_NO_VERIFY");
    return {};
}
}
#endif
