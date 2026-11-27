#include "piconfdocument.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class PiconfDocumentTests : public QObject {
    Q_OBJECT
  private slots:
    void resolvesIncludesAndUpdatesOriginalAssignment() {
        const QString theme = "config/theme.piconf", addon = "addon-data/org.example.timer/config.piconf";
        PiconfDocument::Files files{
            {PiconfDocument::MainPath,
             "# my workspace\r\nformat = 1\r\ninclude = [\"theme.piconf\"]\r\n[appearance]\r\ncolour = "
             "\"${vars.accent}\" # shared\r\n[addons.\"org.example.timer\"]\r\nconfig = "
             "\"../addon-data/org.example.timer/config.piconf\"\r\nenabled = false\r\n"},
            {theme, "[vars]\naccent = \"${vars.base}\"\nbase = \"#123\"   # my colour\ncount = 42\n"},
            {addon, "[settings]\ncolour = \"${vars.accent}\"\ncount = \"${vars.count}\"\nname = \"Clock "
                    "${vars.count}\"\nliteral = \"$${vars.count}\"\n"}};
        PiconfDocument doc;
        QString error;
        QVERIFY2(doc.parse(files, error), qPrintable(error));
        auto fields = doc.values(addon).value("settings").toMap();
        QCOMPARE(fields.value("colour"), "#123");
        QCOMPARE(fields.value("count").typeId(), QMetaType::LongLong);
        QCOMPARE(fields.value("name"), "Clock 42");
        QCOMPARE(fields.value("literal"), "${vars.count}");
        QVERIFY2(doc.setValue(PiconfDocument::MainPath, {"appearance", "colour"}, "#456", error),
                 qPrintable(error));
        QCOMPARE(doc.files()[PiconfDocument::MainPath], files[PiconfDocument::MainPath]);
        QCOMPARE(doc.files()[addon], files[addon]);
        QCOMPARE(doc.files()[theme], QByteArray(files[theme]).replace("#123", "#456"));
        QCOMPARE(doc.values(addon)["settings"].toMap()["colour"], "#456");
    }
    void invalid_data() {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("version") << QByteArray("format=2\n");
        QTest::newRow("duplicate") << QByteArray("format=1\nx=1\nx=2\n");
        QTest::newRow("cycle") << QByteArray("format=1\n[vars]\na=\"${vars.b}\"\nb=\"${vars.a}\"\n");
        QTest::newRow("missing") << QByteArray("format=1\nk=\"${vars.no}\"\n");
        QTest::newRow("type") << QByteArray("format=1\nk=\"X ${vars.a}\"\n[vars]\na=[1]\n");
        QTest::newRow("parent") << QByteArray("format=1\ninclude=[\"../../private.piconf\"]\n");
        QTest::newRow("include-cycle") << QByteArray("format=1\ninclude=[\"pilon.piconf\"]\n");
        QTest::newRow("include-type") << QByteArray("format=1\ninclude=true\n");
        QTest::newRow("invalid-utf8") << QByteArray("format=1\nk=\"\xff\"\n");
        QTest::newRow("section-value-collision") << QByteArray("format=1\na=3\n[a]\nb=true\n");
        QTest::newRow("wrong-addon-directory")
            << QByteArray("format=1\n[addons.ssh]\nconfig=\"../addon-data/notes/config.piconf\"\n");
    }
    void invalid() {
        QFETCH(QByteArray, body);
        PiconfDocument doc;
        QString error;
        QVERIFY(doc.parse({{PiconfDocument::MainPath, "format = 1\n"}}, error));
        QVERIFY(!doc.parse({{PiconfDocument::MainPath, body}}, error));
        QVERIFY2(error.contains(PiconfDocument::MainPath), qPrintable(error));
        QCOMPARE(doc.files().value(PiconfDocument::MainPath), "format = 1\n");
    }
    void batchIntroducesAddonAndKeepsInlineReferences() {
        PiconfDocument doc;
        QString error;
        const auto path = PiconfDocument::MainPath;
        const QString addon = "addon-data/org.example.test/config.piconf";
        QVERIFY2(doc.parse({{path, R"(# workspace
format=1
[vars]
base = 32
[settings]
obj = {"height":"${vars.base}", "draft":"saved"} # retained
)"}},
                           error),
                 qPrintable(error));
        QVERIFY2(doc.apply({{path, {"settings", "obj", "draft"}, "edited"},
                            {path, {"addons", "org.example.test", "config"}, "../" + addon},
                            {path, {"addons", "org.example.test", "enabled"}, false}},
                           {{addon, "format=1\n"}}, error),
                 qPrintable(error));
        QCOMPARE(doc.addonFile("org.example.test"), addon);
        QVERIFY(doc.files().value(path).contains("${vars.base}"));
        QVERIFY(doc.files().value(path).contains("# retained"));
        QVERIFY(doc.setValue(path, {"vars", "base"}, 64, error));
        QCOMPARE(doc.values()["settings"].toMap()["obj"].toMap()["height"].toInt(), 64);
        QCOMPARE(doc.values()["settings"].toMap()["obj"].toMap()["draft"].toString(), "edited");
        QVERIFY(doc.setValue(path, {"settings", "obj", "draft"}, QVariant(), error));
        QVERIFY(!doc.values()["settings"].toMap()["obj"].toMap().contains("draft"));
    }
    void conflictingBatchEditsLeaveSharedAssignmentsUntouched() {
        const auto path = PiconfDocument::MainPath;
        const PiconfDocument::Files original{{path,
            "format=1\n[vars]\nlevel=12\n[appearance]\nfirst=\"${vars.level}\"\nsecond=\"${vars.level}\"\n"}};
        PiconfDocument doc;
        QString error;
        QVERIFY(doc.parse(original, error));
        QVERIFY(!doc.apply({{path, {"appearance", "first"}, 21}, {path, {"appearance", "second"}, 22}}, {}, error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(doc.files(), original);
        QVERIFY2(doc.apply({{path, {"appearance", "first"}, 21}, {path, {"appearance", "second"}, 21}}, {}, error),
                 qPrintable(error));
        const auto appearance = doc.values().value("appearance").toMap();
        QCOMPARE(appearance.value("first").toInt(), 21);
        QCOMPARE(appearance.value("second").toInt(), 21);
    }
    void cannotLoadForeignAddonIncludes() {
        const QString own = "addon-data/org.example.a/config.piconf",
                      other = "addon-data/org.example.b/settings.piconf";
        PiconfDocument doc;
        QString error;
        QVERIFY(!doc.parse(
            {{PiconfDocument::MainPath,
              "format=1\n[addons.\"org.example.a\"]\nconfig=\"../addon-data/org.example.a/config.piconf\""},
             {own, "include=[\"../org.example.b/settings.piconf\"]"},
             {other, "x=2"}},
            error));
        QVERIFY2(error.contains("another add-on"), qPrintable(error));
    }
    void safeDiskReadsAndRoundTrip() {
        PiconfDocument doc;
        QString error;
        QTemporaryDir tmp;
        QVERIFY(QDir(tmp.path()).mkpath("config"));
        QFile file(tmp.filePath(PiconfDocument::MainPath));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QVariantMap settings{
            {"format", 1},
            {"test", QVariantMap{{"a.b", QVariantList{QStringLiteral("value"), 2, true}}, {"中文", "λ"}}}};
        file.write(PiconfDocument::encode(settings));
        file.close();
        QVERIFY2(doc.load(tmp.path(), error), qPrintable(error));
        QCOMPARE(doc.values()["test"].toMap()["中文"], "λ");
        QVERIFY(!doc.parse({{PiconfDocument::MainPath, "format=1\n"}, {"config/forgotten.piconf", "x=1\n"}},
                           error));
        QVERIFY(file.remove());
        QFile external(tmp.filePath("private.txt"));
        QVERIFY(external.open(QIODevice::WriteOnly));
        external.write("format=1\n");
        external.close();
        QVERIFY(QFile::link(external.fileName(), file.fileName()));
        QVERIFY(!doc.load(tmp.path(), error));
    }
};
QTEST_GUILESS_MAIN(PiconfDocumentTests)
#include "piconfdocument_tests.moc"
