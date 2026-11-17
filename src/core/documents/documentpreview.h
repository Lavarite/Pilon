#pragma once
#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QRectF>
#include <QUrl>
#include <QStandardItemModel>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <atomic>

class DocumentSections : public QStandardItemModel {
public:
    explicit DocumentSections(QObject *parent) : QStandardItemModel(parent) {}
    QHash<int, QByteArray> roleNames() const override {
        return {{Qt::DisplayRole, "display"}, {Qt::UserRole, "pageNumber"}, {Qt::UserRole + 1, "destinationY"}};
    }
};

// Owns page layout, selection and render scheduling for the custom Qt viewer.
class DocumentPreview : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(int pageCount READ rowCount NOTIFY changed)
    Q_PROPERTY(qreal pageWidth READ pageWidth NOTIFY changed)
    Q_PROPERTY(qreal totalHeight READ totalHeight NOTIFY changed)
    Q_PROPERTY(QVariantList sections READ sections NOTIFY changed)
    Q_PROPERTY(QAbstractItemModel *sectionModel READ sectionModel CONSTANT)
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectionChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchChanged)
public:
    explicit DocumentPreview(QObject *parent = nullptr);
    ~DocumentPreview() override;
    enum Role { Width = Qt::UserRole + 1, Height, Top, Image, Highlights, PageError };
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QUrl source() const { return m_source; }
    void setSource(const QUrl &source);
    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    qreal pageWidth() const { return m_width; }
    qreal totalHeight() const { return m_height; }
    QVariantList sections() const { return m_sections; }
    QAbstractItemModel *sectionModel() { return &m_sectionModel; }
    QString selectedText() const;
    bool searching() const { return m_searching; }
    static QVariantMap decodeDocument(const QVariantMap &request, const std::atomic_bool *cancel);
    Q_INVOKABLE qreal pageY(int page, qreal scale) const;
    Q_INVOKABLE int pageAt(qreal y, qreal scale) const;
    Q_INVOKABLE void prioritize(qreal top, qreal bottom, qreal scale);
    Q_INVOKABLE void select(qreal x, qreal y, qreal contentWidth, qreal scale, bool begin);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void copy() const;
    Q_INVOKABLE qreal selectionY(qreal scale) const;
    Q_INVOKABLE QVariantMap linkAt(qreal x, qreal y, qreal contentWidth, qreal scale) const;
    Q_INVOKABLE void find(const QString &text, int fromPage, bool backwards = false);
signals:
    void changed();
    void selectionChanged();
    void pageRendered(int page);
    void found(int page);
    void searchChanged();
private:
    struct Page {
        QSizeF size;
        qreal top = 0;
        QUrl image;
        QString text, error;
        QList<QRectF> characters;
        QVariantList links;
        int resolution = 0;
    };
    struct State;
    struct Result {
        std::shared_ptr<State> state;
        QList<Page> pages;
        QVariantList sections;
        QString error;
        int page = -1;
        quint64 generation = 0;
        int offset = -1, length = 0;
    };
    static Page decodedPage(const QVariantMap &map);
    void schedule();
    void notifySelection();
    void search(const QString &text, int fromPage, bool backwards, bool all);
    std::pair<int, int> position(qreal x, qreal y, qreal contentWidth, qreal scale) const;
    std::pair<int, int> range(int page) const;
    QFutureWatcher<Result> m_worker;
    QFutureWatcher<Result> m_searchWorker;
    std::shared_ptr<std::atomic_bool> m_searchCancel;
    std::shared_ptr<State> m_state;
    QList<Page> m_pages;
    QList<int> m_visible;
    QVariantList m_sections;
    DocumentSections m_sectionModel{this};
    QUrl m_source;
    QString m_error;
    bool m_loading = false, m_working = false;
    bool m_searching = false;
    quint64 m_generation = 0;
    qreal m_width = 0, m_height = 0, m_scale = 1;
    std::pair<int, int> m_anchor{-1, 0}, m_cursor{-1, 0};
};
