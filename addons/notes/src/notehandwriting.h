#pragma once
#include <QObject>
#include <QProcess>
#include <QFutureWatcher>
#include <QTimer>
#include <QTemporaryDir>
#include <QVariantMap>
#include <QMap>
#include <QRectF>
#include <memory>

// Rasterization happens on a worker thread; the local recognizer is a separate,
// persistent process. Only finished, matching revisions enter the search index.
class NoteHandwriting : public QObject {
    Q_OBJECT
public:
    explicit NoteHandwriting(QObject *parent = nullptr);
    ~NoteHandwriting() override;
    void enqueue(const QString &id, const QVariantList &strokes, qint64 revision = 0, const QVariantList &previousLines = {}, const QVariantList &elements = {}, const QString &assetRoot = {});
    void setWriting(bool writing);
    void setActive(bool active);
    static QString fingerprint(const QVariantList &strokes);
    static QVariantList renderLines(const QVariantList &strokes, const QString &directory);
    struct Cache { QHash<QString, QVariantList> tiles; };
    static QVariantList prepareLines(const QVariantList &strokes, const QString &directory, Cache &cache, const QVariantList &previousLines = {}, const QVariantList &elements = {}, const QString &assetRoot = {});
signals:
    void indexed(QString id, qint64 revision, QVariantList lines);
    void failed(QString message);
private:
    void next();
    void send();
    void finish();
    void publish();
    QTimer m_delay, m_timeout;
    QProcess m_process;
    QFutureWatcher<QVariantList> m_render;
    QTemporaryDir m_directory;
    struct Job { QVariantList strokes, previousLines; qint64 revision; QVariantList elements; QString assetRoot; };
    QMap<QString, Job> m_pending;
    QMap<QString, std::shared_ptr<Cache>> m_caches;
    qint64 m_revision = 0;
    bool m_writing = false, m_active = true;
    QString m_id, m_root;
    QByteArray m_output, m_request;
    QVariantList m_lines;
};
