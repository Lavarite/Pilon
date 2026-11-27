#include "terminalitem.h"

#include <QClipboard>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QProcess>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

namespace {
int resizeProbe(const QString &scenario)
{
    TerminalItem terminal;
    const QFontMetricsF metrics(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    const auto grid = [&](int columns, int rows) {
        terminal.setSize({28 + columns * std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))),
                          28 + rows * std::ceil(metrics.height())});
    };
    grid(8, 4);
    for (int line = 0; line < 40; ++line) {
        const QString prefix = scenario == "unicode" ? QString::fromUtf8("界é") : QStringLiteral("ROW");
        terminal.feed((prefix + QString::number(line) + "\r\n").toUtf8());
    }
    // Widening preserves narrower saved lines; increasing rows must pop and pad them.
    grid(32, 4);
    grid(32, 12);
    if (scenario == "cycles") {
        for (int pass = 0; pass < 20; ++pass) {
            grid(8, 4);
            terminal.feed("cycle\r\n");
            grid(80, 1);
            grid(32, 12);
        }
    }
    terminal.selectAll();
    terminal.copySelection();
    const QString restored = QGuiApplication::clipboard()->text();
    if (!restored.contains(scenario == "unicode" ? QString::fromUtf8("界é39") : QStringLiteral("ROW39"))) {
        fprintf(stderr, "Unexpected owned fixture copy: %s\n", restored.toUtf8().constData());
        return 1;
    }
    terminal.feed("\r\nPILON_ALIVE");
    terminal.selectAll();
    terminal.copySelection();
    return QGuiApplication::clipboard()->text().contains("PILON_ALIVE") ? 0 : 2;
}
}

class TerminalTests : public QObject {
    Q_OBJECT
private slots:
    void paintsWideGlyphsAsSingleCharacters()
    {
        TerminalItem terminal;
        terminal.setSize({320, 160});
        terminal.setBackgroundColor(Qt::black);
        terminal.setForegroundColor(Qt::white);
        terminal.feed(QStringLiteral("界🙂Z").toUtf8());
        QImage actual(320, 160, QImage::Format_RGB32);
        actual.fill(Qt::black);
        {
            QPainter painter(&actual);
            terminal.paint(&painter);
        }
        const auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        const QFontMetricsF metrics(font);
        const qreal width = std::ceil(metrics.horizontalAdvance(QLatin1Char('M')));
        const qreal height = std::ceil(metrics.height());
        const qreal baseline = 14 + (height - metrics.height()) / 2 + metrics.ascent();
        QImage expected(320, 160, QImage::Format_RGB32);
        expected.fill(Qt::black);
        {
            QPainter painter(&expected);
            painter.setFont(font);
            painter.setPen(Qt::white);
            painter.drawText(QPointF(14, baseline), QStringLiteral("界"));
            painter.drawText(QPointF(14 + 2 * width, baseline), QStringLiteral("🙂"));
            painter.drawText(QPointF(14 + 4 * width, baseline), QStringLiteral("Z"));
        }
        const QRect line(14, 14, std::ceil(8 * width), std::ceil(height));
        QCOMPARE(actual.copy(line), expected.copy(line));
    }

    void paintsAnsiIndexedAndTrueColors()
    {
        TerminalItem terminal;
        terminal.setSize({320, 160});
        terminal.setBackgroundColor(Qt::black);
        terminal.setForegroundColor(Qt::white);
        terminal.feed("\x1b[32;44mA\x1b[38;5;208;48;5;27mB"
                      "\x1b[38;2;14;204;170;48;2;96;30;123mC\x1b[0mD");

        QImage actual(320, 160, QImage::Format_RGB32);
        actual.fill(Qt::black);
        {
            QPainter painter(&actual);
            terminal.paint(&painter);
        }
        const auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        const QFontMetricsF metrics(font);
        const int width = std::ceil(metrics.horizontalAdvance(QLatin1Char('M')));
        const int height = std::ceil(metrics.height());
        const qreal baseline = 14 + (height - metrics.height()) / 2 + metrics.ascent();
        QImage expected(320, 160, QImage::Format_RGB32);
        expected.fill(Qt::black);
        {
            QPainter painter(&expected);
            painter.setFont(font);
            const QColor backgrounds[] = {{0, 0, 224}, {0, 51, 255}, {96, 30, 123}, Qt::black};
            const QColor foregrounds[] = {{0, 224, 0}, {255, 102, 0}, {14, 204, 170}, Qt::white};
            for (int column = 0; column < 4; ++column) {
                const int left = 14 + column * width;
                painter.fillRect(QRect(left, 14, width, height), backgrounds[column]);
                painter.setPen(foregrounds[column]);
                painter.drawText(QPointF(left, baseline), QString(QLatin1Char('A' + column)));
            }
        }
        const QRect line(14, 14, 4 * width, height);
        QCOMPARE(actual.copy(line), expected.copy(line));
    }

    void ctrlCCopiesSelectionAndOtherwiseInterrupts()
    {
        QQuickWindow window;
        TerminalItem terminal(window.contentItem());
        window.resize(320, 160);
        terminal.setSize({320, 160});
        terminal.setActive(true);
        terminal.feed("copy this");
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        const QFontMetricsF metrics(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        const int width = std::ceil(metrics.horizontalAdvance(QLatin1Char('M')));
        const QPoint left(15, 15), right(15 + 3 * width, 15);
        QTest::mousePress(&window, Qt::LeftButton, {}, left);
        QTest::mouseMove(&window, right);
        QTest::mouseRelease(&window, Qt::LeftButton, {}, right);
        QVERIFY(terminal.hasSelection());
        QVERIFY(terminal.hasActiveFocus());

        auto clipboard = QGuiApplication::clipboard();
        clipboard->setText("previous clipboard contents");
        QSignalSpy output(&terminal, &TerminalItem::dataGenerated);
        QTest::keyClick(&window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(clipboard->text(), QStringLiteral("copy"));
        QCOMPARE(output.size(), 0);
        QVERIFY(terminal.hasSelection());

        clipboard->setText("previous clipboard contents");
        QTest::keyClick(&window, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(clipboard->text(), QStringLiteral("copy"));
        QCOMPARE(output.size(), 0);
        QVERIFY(terminal.hasSelection());

        QTest::mouseClick(&window, Qt::LeftButton, {}, left);
        QVERIFY(!terminal.hasSelection());
        QTest::keyClick(&window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(output.size(), 1);
        QCOMPARE(output.first().first().toByteArray(), QByteArray(1, '\x03'));
        QCOMPARE(clipboard->text(), QStringLiteral("copy"));
    }

    void clearingScrollbackClearsSelection()
    {
        TerminalItem terminal;
        terminal.setSize({320, 160});
        terminal.feed(QByteArray("history\r\n").repeated(80));
        terminal.selectAll();
        QVERIFY(terminal.hasSelection());
        terminal.feed("\x1b[3J");
        QVERIFY(!terminal.hasSelection());
        QCOMPARE(terminal.scrollPosition(), 0);
    }
    void trackpadPixelDeltasAccumulate()
    {
        TerminalItem terminal;
        terminal.setSize({320, 160});
        terminal.feed(QByteArray("history\r\n").repeated(80));
        for (int index = 0; index < 100; ++index) {
            QWheelEvent event({}, {}, {0, 1}, {}, Qt::NoButton, Qt::NoModifier,
                              Qt::ScrollUpdate, false);
            QCoreApplication::sendEvent(&terminal, &event);
        }
        QVERIFY(terminal.scrollPosition() > 0);
        QWheelEvent wheel({}, {}, {}, {0, -120}, Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        const int before = terminal.scrollPosition();
        QCoreApplication::sendEvent(&terminal, &wheel);
        QCOMPARE(terminal.scrollPosition(), qMax(0, before - 3));
    }
    void scrollbackResizeCompletes_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"widen", "unicode", "cycles"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void scrollbackResizeCompletes()
    {
        QFETCH(QString, scenario);
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {"-platform", "offscreen", "--resize-probe", scenario});
        QVERIFY(child.waitForStarted());
        const bool finished = child.waitForFinished(3000);
        if (!finished) {
            child.kill();
            child.waitForFinished();
        }
        QVERIFY2(finished, "Restoring narrower scrollback after a resize must not block the GUI thread");
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        const QByteArray output = child.readAll();
        QVERIFY2(child.exitCode() == 0, output.constData());
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir configHome;
    if (!configHome.isValid())
        return 1;
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(configHome.path()));
    QGuiApplication application(argc, argv);
    const auto arguments = application.arguments();
    const int probe = arguments.indexOf("--resize-probe");
    if (probe >= 0)
        return resizeProbe(arguments.value(probe + 1));
    TerminalTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "terminal_tests.moc"
