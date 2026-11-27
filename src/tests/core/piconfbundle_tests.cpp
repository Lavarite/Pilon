#include "../../core/workspace/piconfbundle.h"
#include "../../core/workspace/piconftransaction.h"

#include <QCborArray>
#include <QCborMap>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

class PiconfBundleTests : public QObject {
    Q_OBJECT
    using Files = PiconfDocument::Files;
    const QString main = PiconfDocument::MainPath;
    static QByteArray contents(const QString &path) {
        QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
    }
    static void put(const QString &path, const QByteArray &bytes) {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(bytes), bytes.size());
    }
  private slots:
    void roundTripPreservesEditableGraphAndExcludesPrivateState() {
        QTemporaryDir from, to;
        Files files{{main, "format = 1\ninclude = [\"theme.piconf\"]\n[addons.\"org.example.timer\"]\n"
                           "source = \"https://github.com/example/timer\"\nenabled = false\nconfig = \"../addon-data/org.example.timer/config.piconf\"\n"},
                    {"config/theme.piconf", "# keep this\n[vars]\ntint = \"#339944\"\n"},
                    {"addon-data/org.example.timer/config.piconf", "[settings]\ntint = \"${vars.tint}\"\n"}};
        QString error;
        QVERIFY2(PiconfTransaction::commit(from.path(), {}, files, error), qPrintable(error));
        put(from.filePath(".pilon/settings.ini"), "private-marker-never-export");
        PiconfDocument exported, imported;
        QVERIFY2(exported.load(from.path(), error), qPrintable(error));
        const auto archive = QUrl::fromLocalFile(to.filePath("prefs.piconfpack"));
        QVERIFY2(PiconfBundle::save(archive, exported, error), qPrintable(error));
        QVERIFY(!contents(archive.toLocalFile()).contains("private-marker"));
        QVERIFY2(PiconfBundle::open(archive, imported, error), qPrintable(error));
        QCOMPARE(imported.files(), files);
        QVERIFY2(PiconfTransaction::commit(to.path(), {}, imported.files(), error), qPrintable(error));
        PiconfDocument moved;
        QVERIFY2(moved.load(to.path(), error), qPrintable(error));
        QCOMPARE(moved.values("addon-data/org.example.timer/config.piconf")["settings"].toMap()["tint"].toString(), "#339944");

        QVERIFY2(moved.setValue(main, {"application", "keyboardShortcutHints"}, false, error), qPrintable(error));
        const auto returning = QUrl::fromLocalFile(to.filePath("return.PICONFPACK"));
        QVERIFY2(PiconfBundle::save(returning, moved, error), qPrintable(error));
        PiconfDocument restored;
        QVERIFY2(PiconfBundle::open(returning, restored, error), qPrintable(error));
        QCOMPARE(restored.files(), moved.files());
        QVERIFY2(PiconfTransaction::commit(from.path(), files, restored.files(), error), qPrintable(error));
        QVERIFY2(restored.load(from.path(), error), qPrintable(error));
        QCOMPARE(restored.values().value("application").toMap().value("keyboardShortcutHints"), QVariant(false));
        QCOMPARE(restored.files().value("config/theme.piconf"), files.value("config/theme.piconf"));
        QCOMPARE(contents(from.filePath(".pilon/settings.ini")), "private-marker-never-export");
    }

    void rejectInvalidBundleMembersWithoutTouchingWorkspace_data() {
        QTest::addColumn<QString>("member");
        for (const auto &path : {"/outside.piconf","config/../outside.piconf","config\\outside.piconf",
                                 ".pilon/settings.ini","config/pilon.piconf","config/unreferenced.piconf"})
            QTest::newRow(path) << QString::fromLatin1(path);
    }
    void rejectInvalidBundleMembersWithoutTouchingWorkspace() {
        QFETCH(QString, member);
        QTemporaryDir root; QString error;
        QJsonArray entries{QJsonObject{{"path",main},{"text","format = 1\n"}},
                           QJsonObject{{"path",member},{"text","arbitrary = true\n"}}};
        auto bundle = root.filePath("input.piconfpack");
        put(bundle,QJsonDocument(QJsonObject{{"pilonConfigurationBundle",1},{"files",entries}}).toJson());
        PiconfDocument parsed;
        QVERIFY(!PiconfBundle::open(QUrl::fromLocalFile(bundle), parsed, error));
        QVERIFY(!error.isEmpty()); QVERIFY(!QFile::exists(root.filePath(main)));
    }

    void staleOrLinkedTargetCannotOverwriteOtherFiles() {
        QTemporaryDir root, outside;
        QString error;
        Files original{{main,"old"}}, changes{{main,"new"},{"addon-data/org.example.timer/config.piconf","value"}};
        QVERIFY(PiconfTransaction::commit(root.path(), {}, original, error));
        put(root.filePath(main),"external edit");
        QVERIFY(!PiconfTransaction::commit(root.path(), original, changes, error));
        QCOMPARE(contents(root.filePath(main)),"external edit");
        QVERIFY(!QFile::exists(root.filePath("addons/org.example.timer/data/config.piconf")));
        QVERIFY(QDir().mkpath(root.filePath("addons")));
        QVERIFY(QFile::link(outside.path(), root.filePath("addons/org.example.timer")));
        original[main]="external edit";
        QVERIFY(!PiconfTransaction::commit(root.path(),original,changes,error));
        QVERIFY(!QFile::exists(outside.filePath("config.piconf")));
        QCOMPARE(contents(root.filePath(main)),"external edit");
    }

    void exportProtectsEditableConfiguration() {
        QTemporaryDir root;
        QString error;
        PiconfDocument doc;
        QVERIFY(doc.parse({{main, "format = 1\n"}}, error));
        put(root.filePath(main), "hand-edited");
        QVERIFY(!PiconfBundle::save(QUrl::fromLocalFile(root.filePath(main)), doc, error));
        QCOMPARE(contents(root.filePath(main)), "hand-edited");
        const auto extensionless = root.filePath("Settings");
        QVERIFY(!PiconfBundle::save(QUrl::fromLocalFile(extensionless), doc, error));
        QVERIFY(!QFile::exists(extensionless));
    }

    void interruptedUpdateRestoresPreviousGraphAndNativeSettings() {
        QTemporaryDir root;
        const auto old = QByteArray("format = 1\n"), replaced = QByteArray("format = 1\nmode = 2\n");
        const auto native = QStringLiteral(".pilon/settings.ini");
        const auto journal = root.filePath(".pilon/piconf-transaction");
        const auto hash = [](const QByteArray &b) { return QCryptographicHash::hash(b,QCryptographicHash::Sha256); };
        put(root.filePath(main),replaced); put(root.filePath(native),"new");
        const QCborMap undo{{main,QCborArray{old,hash(replaced)}}, {native,QCborArray{QCborValue(nullptr),hash("new")}}};
        put(journal,QCborValue(undo).toCbor());
        QString error;
        QVERIFY2(PiconfTransaction::recover(root.path(),error),qPrintable(error));
        QCOMPARE(contents(root.filePath(main)),old); QVERIFY(!QFile::exists(root.filePath(native)));
        QVERIFY(!QFile::exists(journal)); QVERIFY(PiconfTransaction::recover(root.path(),error));
    }
};
QTEST_GUILESS_MAIN(PiconfBundleTests)
#include "piconfbundle_tests.moc"
