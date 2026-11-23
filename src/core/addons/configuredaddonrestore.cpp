#include "addonregistry.h"

#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTemporaryFile>
#include <memory>

bool AddonRegistry::downloadConfigured(const QUrl &source, const QString &id, const QString &version,
                                       const QString &sha256) {
    if (m_download)
        return fail(tr("Finish the current add-on download first."));
    if (id.isEmpty() || !source.isValid() || source.scheme() != "https" || source.host().isEmpty() ||
        !source.userInfo().isEmpty() || source.hasFragment() || !source.path().endsWith(".piext", Qt::CaseInsensitive))
        return fail(tr("Choose an HTTPS .piext release asset."));
    if (find(id))
        return fail(tr("This add-on is already installed."));
    auto *network = new QNetworkAccessManager(this);
    auto *file = new QTemporaryFile(QDir::temp().filePath("pilon-restore-XXXXXX.piext"), network);
    if (!file->open()) {
        network->deleteLater();
        return fail(tr("Cannot create a temporary package file."));
    }

    QNetworkRequest request(source);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    auto *reply = network->get(request);
    reply->setReadBufferSize(256 * 1024);
    m_download = reply;
    m_restoring = id;
    const auto failure = std::make_shared<QString>();
    const auto consume = [reply, file, failure] {
        constexpr qint64 limit = 128 * 1024 * 1024;
        while (failure->isEmpty() && reply->bytesAvailable()) {
            const auto bytes = reply->read(64 * 1024);
            if (bytes.isEmpty() || file->size() + bytes.size() > limit || file->write(bytes) != bytes.size()) {
                *failure = QObject::tr("Cannot store the download. Packages must fit on disk and be at most 128 MiB.");
                return false;
            }
        }
        return failure->isEmpty();
    };
    connect(reply, &QIODevice::readyRead, this, [reply, consume] {
        if (!consume()) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [=, this] {
        bool installed = false;
        if (failure->isEmpty() && reply->error() != QNetworkReply::NoError) {
            if (reply->error() != QNetworkReply::OperationCanceledError)
                *failure = tr("Cannot download the add-on: %1").arg(reply->errorString());
        } else if (failure->isEmpty() && consume()) {
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 || !file->flush())
                *failure = tr("The server did not provide a complete package.");
            else {
                file->close();
                installed = installConfigured(QUrl::fromLocalFile(file->fileName()), id, version, sha256);
                if (!installed) *failure = error();
            }
        }
        m_download.clear();
        m_restoring.clear();
        emit restoreChanged();
        emit configuredDownloadFinished(id, installed, *failure);
        network->deleteLater();
    });
    emit restoreChanged();
    return true;
}

void AddonRegistry::cancelConfiguredDownload() {
    if (m_download) m_download->abort();
}
