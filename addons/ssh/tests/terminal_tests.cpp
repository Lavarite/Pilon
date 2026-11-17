#include "terminalitem.h"

#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QProcess>
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
    QGuiApplication application(argc, argv);
    const auto arguments = application.arguments();
    const int probe = arguments.indexOf("--resize-probe");
    if (probe >= 0)
        return resizeProbe(arguments.value(probe + 1));
    TerminalTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "terminal_tests.moc"
