#include "../../core/documents/documentpreview.h"
#include <QTest>
#include <QSignalSpy>
#include <QPdfWriter>
#include <QPainter>
#include <QTemporaryDir>
#include <QClipboard>
#include <QGuiApplication>

class DocumentPreviewTests : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QString makePdf(const QString &name, int pages) {
        const auto path = directory.filePath(name);
        QPdfWriter writer(path); writer.setResolution(72); writer.setPageSize(QPageSize(QPageSize::A4));
        QPainter painter(&writer); painter.setFont(QFont("DejaVu Sans", 16));
        for (int i = 0; i < pages; ++i) {
            if (i) writer.newPage();
            painter.drawText(QPointF(30, 50), QString("Page %1 selectable text").arg(i + 1));
            painter.drawText(QPointF(30, 80), "Another selectable text match");
        }
        painter.end(); return path;
    }
private slots:
    void namedDestinationsUseTopLeftCoordinates() {
        DocumentPreview document;
        document.setSource(QUrl::fromLocalFile(QFINDTESTDATA("../fixtures/named-section.pdf")));
        QTRY_COMPARE(document.rowCount(), 1);
        const auto section = document.sections().first().toMap();
        QCOMPARE(section.value("page").toInt(), 0);
        QCOMPARE(section.value("top").toDouble(), .25);
    }

    void sectionsAndLinksKeepPageGeometry() {
        DocumentPreview document;
        document.setSource(QUrl::fromLocalFile(QFINDTESTDATA("../fixtures/viewer.pdf")));
        QTRY_COMPARE(document.rowCount(), 2);
        QCOMPARE(document.sections().first().toMap().value("page").toInt(), 0);
        auto *tree = document.sectionModel();
        QCOMPARE(tree->rowCount(), 2);
        QCOMPARE(tree->rowCount(tree->index(0, 0)), 1);
        QCOMPARE(tree->rowCount(tree->index(0, 0, tree->index(0, 0))), 1);
        QCOMPARE(document.data(document.index(0), DocumentPreview::Width).toInt(), 400);
        QCOMPARE(document.data(document.index(1), DocumentPreview::Width).toInt(), 600);
        QTRY_VERIFY(!document.data(document.index(0), DocumentPreview::Image).toUrl().isEmpty());
        // First page is centred in a 600-point-wide document.
        QCOMPARE(document.linkAt(160, 70, 600, 1).value("page").toInt(), 1);
        QVERIFY(document.linkAt(110, 300, 600, 1).isEmpty());
    }
    void visiblePagesPreemptSequentialRendering() {
        DocumentPreview document; QSignalSpy rendered(&document, &DocumentPreview::pageRendered);
        connect(&document, &DocumentPreview::pageRendered, &document, [&](int page) {
            if (page == 0) { const auto y = document.pageY(24, 1); document.prioritize(y, y + 100, 1); }
        });
        document.setSource(QUrl::fromLocalFile(makePdf("large.pdf", 30)));
        QTRY_COMPARE(document.rowCount(), 30);
        const qreal top = document.pageY(24, 1);
        QTRY_VERIFY_WITH_TIMEOUT(rendered.size() >= 3, 10000);
        QCOMPARE(rendered.at(0).at(0).toInt(), 0);
        QCOMPARE(rendered.at(1).at(0).toInt(), 24);
        QCOMPARE(rendered.at(2).at(0).toInt(), 1);
        QCOMPARE(document.pageAt(top + 20, 1), 24);
        QCOMPARE(document.pageAt(document.pageY(24, 2) + 20, 2), 24);
        QVERIFY(!document.data(document.index(24), DocumentPreview::Image).toUrl().isEmpty());
        document.setSource({}); QCOMPARE(document.rowCount(), 0); QVERIFY(!document.loading());
    }
    void searchAndSelectionDoNotWaitForRasterization() {
        DocumentPreview document; QSignalSpy found(&document, &DocumentPreview::found);
        document.setSource(QUrl::fromLocalFile(makePdf("search.pdf", 40)));
        QTRY_COMPARE(document.rowCount(), 40);
        document.find("Page 40", 0);
        QTRY_COMPARE_WITH_TIMEOUT(found.size(), 1, 10000);
        QCOMPARE(found.first().first().toInt(), 39);
        QCOMPARE(document.selectedText(), QString("Page 40"));
        const auto boxes = document.data(document.index(39), DocumentPreview::Highlights).toList(); QVERIFY(!boxes.isEmpty());
        document.copy(); QCOMPARE(QGuiApplication::clipboard()->text(), QString("Page 40"));
        document.selectAll(); QTRY_VERIFY_WITH_TIMEOUT(!document.searching(), 10000);
        QVERIFY(document.selectedText().contains("Page 1 selectable text"));
        QVERIFY(document.selectedText().contains("Page 40 selectable text"));
    }
    void characterSelectionAndRepeatedSearch() {
        DocumentPreview document; QSignalSpy found(&document, &DocumentPreview::found);
        document.setSource(QUrl::fromLocalFile(makePdf("text.pdf", 1)));
        QTRY_COMPARE(document.rowCount(), 1);
        document.find("selectable", 0); QTRY_COMPARE(found.size(), 1);
        auto boxes = document.data(document.index(0), DocumentPreview::Highlights).toList(); QVERIFY(boxes.size() > 3);
        const auto first = boxes.first().toRectF(), last = boxes.last().toRectF();
        document.select(first.left(), first.center().y(), document.pageWidth(), 1, true);
        document.select(last.right(), last.center().y(), document.pageWidth(), 1, false);
        QCOMPARE(document.selectedText(), QString("selectable"));
        document.find("selectable", 0); QTRY_COMPARE(found.size(), 2);
        boxes = document.data(document.index(0), DocumentPreview::Highlights).toList();
        QVERIFY(boxes.first().toRectF().top() > first.top());
    }
    void switchingDocumentsDiscardsStaleWork() {
        DocumentPreview document;
        document.setSource(QUrl::fromLocalFile(makePdf("old.pdf", 25)));
        document.setSource(QUrl::fromLocalFile(makePdf("new.pdf", 2)));
        QTRY_VERIFY_WITH_TIMEOUT(!document.loading(), 10000); QCOMPARE(document.rowCount(), 2);
        document.setSource(QUrl::fromLocalFile(directory.filePath("missing.pdf")));
        QTRY_VERIFY_WITH_TIMEOUT(!document.loading(), 10000); QVERIFY(!document.error().isEmpty());
    }
};
QTEST_MAIN(DocumentPreviewTests)
#include "documentpreview_tests.moc"
