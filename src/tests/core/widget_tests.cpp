#include "widgetstore.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QUrlQuery>
#include <QJSEngine>
#include <QQmlEngine>
#include <QScopeGuard>
#include <limits>
#include <unistd.h>
#include <sys/stat.h>
#include <QJsonArray>

class WidgetTests : public QObject {
    Q_OBJECT
    QVariantList providers() const { return {QVariantMap{{"addonId", "org.example.fixture"}, {"widgetId", "list"}, {"name", "Fixture"}}}; }
    QVariantMap snapshot(double generation = 1) const {
        return {{"version", 1}, {"generation", generation}, {"updatedAt", "2026-11-09T12:00:00Z"},
            {"validFrom", "2026-11-09T00:00:00Z"}, {"validThrough", "2026-11-10T00:00:00Z"},
            {"palette", QVariantMap{{"background", "#241e1a"}, {"text", "#d3c4b5"}, {"accent", "#ffb86c"}}},
            {"timeline", QVariantList{QVariantMap{{"at", "2026-11-09T00:00:00Z"}, {"title", "Monday"},
                {"rows", QVariantList{QVariantMap{{"id", "row1"}, {"title", "Readable title"}, {"detail", "12:00"}, {"actionId", "item1"}}}}}}},
            {"actions", QVariantMap{{"open", QVariantMap{}}, {"item1", QVariantMap{{"item", "owned-id"}}}}}};
    }
    QString dataPath(const QString &directory) const {
        const auto name = QCryptographicHash::hash("org.example.fixture/list", QCryptographicHash::Sha256).toHex();
        return directory + '/' + QString::fromLatin1(name) + ".json";
    }
    QByteArray bytes(const QString &path) const { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{}; }
    void write(const QString &path, const QByteArray &data) {
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(data), data.size());
    }
    QUrl action(const WidgetStore &store, const QString &name = "item1", const QString &generation = "1") const {
        QUrl url("pilon://widget/org.example.fixture/list"); QUrlQuery query;
        query.addQueryItem("workspace", store.workspaceId()); query.addQueryItem("action", name); query.addQueryItem("generation", generation); url.setQuery(query); return url;
    }
    qint64 now() const { return QDateTime::fromString("2026-11-09T12:00:00Z", Qt::ISODate).toMSecsSinceEpoch(); }
private slots:
    void publicationSurvivesRestartAndKeepsFailedWrites() {
        QTemporaryDir temp; const auto directory = temp.path() + "/widgets";
        {
            WidgetStore store(directory); QVERIFY(store.configure(temp.path(), providers()));
            QVERIFY(store.publish("org.example.fixture", "list", snapshot()));
            QCOMPARE(store.generation("org.example.fixture", "list"), 1);
            const auto before = bytes(dataPath(directory)); QVERIFY(!before.isEmpty());
            QVERIFY(!store.publish("org.example.other", "list", snapshot(2)));
            QVERIFY(!store.publish("org.example.fixture", "list", snapshot()));
            auto invalid = snapshot(2); invalid["command"] = "untrusted";
            QVERIFY(!store.publish("org.example.fixture", "list", invalid));
            QCOMPARE(bytes(dataPath(directory)), before);
        }
        WidgetStore reopened(directory); QVERIFY(reopened.configure(temp.path(), providers()));
        QCOMPARE(reopened.generation("org.example.fixture", "list"), 1);
    }
    void disableAndWorkspaceSwitchRemoveOwnedDataOnly() {
        QTemporaryDir temp; const auto directory = temp.path() + "/widgets"; WidgetStore store(directory);
        QVERIFY(store.configure(temp.path(), providers())); QVERIFY(store.publish("org.example.fixture", "list", snapshot()));
        QFile unrelated(directory + "/unrelated.txt"); QVERIFY(unrelated.open(QIODevice::WriteOnly)); unrelated.write("keep"); unrelated.close();
        QVERIFY(store.configure(temp.path(), {})); QVERIFY(!QFile::exists(dataPath(directory)));
        QCOMPARE(bytes(unrelated.fileName()), QByteArray("keep"));
        QVERIFY(store.configure(temp.path(), providers())); QVERIFY(store.publish("org.example.fixture", "list", snapshot()));
        const auto workspace = store.workspaceId(); QVERIFY(QDir().mkpath(temp.path() + "/other"));
        QVERIFY(store.configure(temp.path() + "/other", providers())); QVERIFY(store.workspaceId() != workspace);
        QVERIFY(!QFile::exists(dataPath(directory))); QCOMPARE(bytes(unrelated.fileName()), QByteArray("keep"));
    }
    void actionsResolveCurrentOwnedDataAndRejectForgery() {
        QTemporaryDir temp; WidgetStore store(temp.path() + "/widgets");
        QVERIFY(store.configure(temp.path(), providers())); QVERIFY(store.publish("org.example.fixture", "list", snapshot()));
        QUrl url("pilon://widget/org.example.fixture/list");
        QUrlQuery query; query.addQueryItem("workspace", store.workspaceId()); query.addQueryItem("action", "item1"); query.addQueryItem("generation", "1"); url.setQuery(query);
        const auto now = QDateTime::fromString("2026-11-09T12:00:00Z", Qt::ISODate).toMSecsSinceEpoch();
        auto action = store.resolve(url, now); QCOMPARE(action.value("addonId").toString(), "org.example.fixture");
        QCOMPARE(action.value("data").toMap().value("item").toString(), "owned-id");
        QVERIFY(store.resolve(url, now + 86400000).isEmpty());
        query.addQueryItem("command", "untrusted"); url.setQuery(query); QVERIFY(store.resolve(url, now).isEmpty());
        url.setQuery(QString{}); QVERIFY(store.resolve(url, now).isEmpty());
    }
    void invalidSnapshotsLeaveTheCommittedDataUntouched_data() {
        QTest::addColumn<QString>("mutation");
        for (const auto *name : {"extra", "version-bool", "version-string", "generation-bool", "generation-string", "generation-zero", "generation-negative", "generation-fraction", "generation-overflow", "generation-nan", "generation-infinity",
            "date-offset", "date-invalid", "date-precision", "date-overflow", "updated-outside", "empty-coverage", "palette-extra", "palette-invalid", "timeline-empty", "timeline-overflow", "frame-extra", "frame-order", "first-frame", "frame-outside", "rows-overflow", "rows-total",
            "row-extra", "row-id", "row-duplicate", "title-control", "detail-control", "title-size", "detail-size", "unpaired-surrogate", "missing-action", "open-missing", "action-id", "action-key", "action-map", "action-values", "action-array", "action-bytes", "action-pointer", "action-infinity", "actions-overflow", "payload-size"}) QTest::newRow(name) << QString::fromLatin1(name);
    }
    void invalidSnapshotsLeaveTheCommittedDataUntouched() {
        QFETCH(QString, mutation);
        QTemporaryDir temp; const auto directory = temp.path() + "/widgets"; WidgetStore store(directory);
        QVERIFY(store.configure(temp.path(), providers())); QVERIFY(store.publish("org.example.fixture", "list", snapshot()));
        const auto before = bytes(dataPath(directory)); auto value = snapshot(2);
        auto frames = value.value("timeline").toList(); auto frame = frames.first().toMap();
        auto rows = frame.value("rows").toList(); auto row = rows.first().toMap(); auto actions = value.value("actions").toMap();
        if (mutation == "extra") value["command"] = "forbidden";
        else if (mutation == "version-bool") value["version"] = true;
        else if (mutation == "version-string") value["version"] = "1";
        else if (mutation.startsWith("generation-")) {
            const QVariantMap cases{{"generation-bool", true}, {"generation-string", "2"}, {"generation-zero", 0}, {"generation-negative", -1}, {"generation-fraction", 1.5}, {"generation-overflow", 9007199254740992.0}, {"generation-nan", std::numeric_limits<double>::quiet_NaN()}, {"generation-infinity", std::numeric_limits<double>::infinity()}};
            value["generation"] = cases.value(mutation);
        } else if (mutation == "date-offset") value["updatedAt"] = "2026-11-09T12:00:00+00:00";
        else if (mutation == "date-invalid") value["updatedAt"] = "2026-02-30T12:00:00Z";
        else if (mutation == "date-precision") value["updatedAt"] = "2026-11-09T12:00:00.1234Z";
        else if (mutation == "date-overflow") value["updatedAt"] = "2026-11-09T99:00:00Z";
        else if (mutation == "updated-outside") value["updatedAt"] = value.value("validThrough");
        else if (mutation == "empty-coverage") value["validThrough"] = value.value("validFrom");
        else if (mutation.startsWith("palette-")) { auto palette = value.value("palette").toMap(); if (mutation == "palette-extra") palette["unknown"] = "#000000"; else palette["text"] = "red"; value["palette"] = palette; }
        else if (mutation == "timeline-empty") frames.clear();
        else if (mutation == "timeline-overflow") { frames.clear(); for (int i = 0; i < 65; ++i) { auto copy = frame; copy["at"] = QDateTime::fromString("2026-11-09T00:00:00Z", Qt::ISODate).addSecs(i * 60).toString(Qt::ISODate); frames.append(copy); } }
        else if (mutation == "frame-extra") frame["script"] = "forbidden";
        else if (mutation == "frame-order") frames.append(frame);
        else if (mutation == "first-frame") frame["at"] = "2026-11-09T01:00:00Z";
        else if (mutation == "frame-outside") frame["at"] = value.value("validThrough");
        else if (mutation == "rows-overflow") { for (int i = 1; i < 65; ++i) { auto copy = row; copy["id"] = QString("row%1").arg(i + 1); rows.append(copy); } }
        else if (mutation == "rows-total") {
            rows.clear(); for (int i = 0; i < 57; ++i) { auto copy = row; copy["id"] = QString("r%1").arg(i); rows.append(copy); }
            frame["rows"] = rows; frames.clear(); for (int i = 0; i < 9; ++i) { auto copy = frame; copy["at"] = QString("2026-11-09T%1:00:00Z").arg(i, 2, 10, QChar('0')); frames.append(copy); }
        } else if (mutation == "row-extra") row["url"] = "forbidden";
        else if (mutation == "row-id") row["id"] = "../other";
        else if (mutation == "row-duplicate") rows.append(row);
        else if (mutation == "title-control") row["title"] = "title\ncontrol";
        else if (mutation == "detail-control") row["detail"] = "detail\tcontrol";
        else if (mutation == "title-size") row["title"] = QString(257, 'x');
        else if (mutation == "detail-size") row["detail"] = QString(513, 'x');
        else if (mutation == "unpaired-surrogate") row["title"] = QString(QChar(0xd800));
        else if (mutation == "missing-action") row["actionId"] = "not-declared";
        else if (mutation == "open-missing") actions.remove("open");
        else if (mutation == "action-id") actions["bad/id"] = QVariantMap{};
        else if (mutation == "action-key") actions["item1"] = QVariantMap{{"bad/key", "value"}};
        else if (mutation == "action-map") actions["item1"] = QVariantMap{{"nested", QVariantMap{{"data", "forbidden"}}}};
        else if (mutation == "action-values") { QVariantMap data; for (int i = 0; i < 9; ++i) data[QString("k%1").arg(i)] = i; actions["item1"] = data; }
        else if (mutation == "action-array") actions["item1"] = QVariantMap{{"array", QVariantList{"forbidden"}}};
        else if (mutation == "action-bytes") actions["item1"] = QVariantMap{{"binary", QByteArray("forbidden")}};
        else if (mutation == "action-pointer") actions["item1"] = QVariantMap{{"object", QVariant::fromValue(static_cast<QObject *>(nullptr))}};
        else if (mutation == "action-infinity") actions["item1"] = QVariantMap{{"number", std::numeric_limits<double>::infinity()}};
        else if (mutation == "actions-overflow") { for (int i = 0; i < 511; ++i) actions[QString("a%1").arg(i)] = QVariantMap{}; }
        else if (mutation == "payload-size") { QVariantMap data; for (int i = 0; i < 8; ++i) data[QString("k%1").arg(i)] = QString(1024, 'x'); for (int i = 0; i < 64; ++i) actions[QString("a%1").arg(i)] = data; }
        if (!mutation.startsWith("timeline-") && mutation != "frame-order" && mutation != "rows-total") {
            if (mutation != "row-duplicate" && mutation != "rows-overflow") rows[0] = row;
            frame["rows"] = rows; frames[0] = frame;
        }
        value["timeline"] = frames; value["actions"] = actions;
        QVERIFY2(!store.publish("org.example.fixture", "list", value), qPrintable(mutation));
        QVERIFY(!store.error().isEmpty()); QCOMPARE(bytes(dataPath(directory)), before); QCOMPARE(store.generation("org.example.fixture", "list"), 1);
    }
    void currentActionsReplaceOlderDataAndRemovedActionsExpire() {
        QTemporaryDir temp; WidgetStore store(temp.path() + "/widgets"); QVERIFY(store.configure(temp.path(), providers()));
        QVERIFY(store.publish("org.example.fixture", "list", snapshot())); const auto old = action(store);
        auto newer = snapshot(2); auto actions = newer.value("actions").toMap(); actions["item1"] = QVariantMap{{"item", "current-data"}}; newer["actions"] = actions;
        QVERIFY(store.publish("org.example.fixture", "list", newer)); QCOMPARE(store.resolve(old, now()).value("data").toMap().value("item").toString(), "current-data");
        QVERIFY(store.resolve(action(store, "item1", "3"), now()).isEmpty());
        newer = snapshot(3); newer["actions"] = QVariantMap{{"open", QVariantMap{}}}; auto frames = newer.value("timeline").toList(); auto frame = frames.first().toMap(); frame["rows"] = QVariantList{}; newer["timeline"] = QVariantList{frame};
        QVERIFY(store.publish("org.example.fixture", "list", newer)); QVERIFY(store.resolve(old, now()).isEmpty());
        QVERIFY(store.configure(temp.path(), {})); QVERIFY(store.resolve(action(store, "open", "3"), now()).isEmpty());
    }
    void forgedUrisAreRejected_data() {
        QTest::addColumn<QString>("mutation");
        for (const auto *name : {"scheme", "authority", "userinfo", "port", "fragment", "extra-segment", "escaped-segment", "duplicate", "extra-query", "workspace", "zero", "fraction", "overflow", "unknown-action"}) QTest::newRow(name) << QString::fromLatin1(name);
    }
    void forgedUrisAreRejected() {
        QFETCH(QString, mutation); QTemporaryDir temp; WidgetStore store(temp.path() + "/widgets"); QVERIFY(store.configure(temp.path(), providers())); QVERIFY(store.publish("org.example.fixture", "list", snapshot()));
        auto url = action(store); QUrlQuery query(url);
        if (mutation == "scheme") url.setScheme("https"); else if (mutation == "authority") url.setHost("other");
        else if (mutation == "userinfo") url.setUserName("user"); else if (mutation == "port") url.setPort(1234);
        else if (mutation == "fragment") url.setFragment("extra"); else if (mutation == "extra-segment") url.setPath(url.path() + "/extra");
        else if (mutation == "escaped-segment") url.setPath("/org.example.fixture/list%2fextra", QUrl::StrictMode);
        else if (mutation == "duplicate") query.addQueryItem("action", "open"); else if (mutation == "extra-query") query.addQueryItem("command", "forbidden");
        else if (mutation == "workspace") { query.removeAllQueryItems("workspace"); query.addQueryItem("workspace", QString(64, '0')); }
        else if (mutation == "unknown-action") { query.removeAllQueryItems("action"); query.addQueryItem("action", "missing"); }
        else { query.removeAllQueryItems("generation"); query.addQueryItem("generation", mutation == "zero" ? "0" : mutation == "fraction" ? "1.0" : "9007199254740992"); }
        url.setQuery(query); QVERIFY(store.resolve(url, now()).isEmpty());
    }
    void filesystemFailuresAndLinksPreserveExistingData() {
        QTemporaryDir temp; const auto directory = temp.path() + "/widgets"; WidgetStore store(directory);
        QVERIFY(store.configure(temp.path(), providers())); QVERIFY(store.publish("org.example.fixture", "list", snapshot()));
        const auto before = bytes(dataPath(directory)), catalogue = bytes(directory + "/catalog.json"); const auto scope = store.workspaceId();
        QVERIFY(QDir().mkpath(temp.path() + "/other")); const auto permissions = QFileInfo(directory).permissions();
        QVERIFY(QFile::setPermissions(directory, QFile::ReadOwner | QFile::ExeOwner));
        const auto restore = qScopeGuard([&] { QFile::setPermissions(directory, permissions); });
        QVERIFY(!store.publish("org.example.fixture", "list", snapshot(2))); QVERIFY(!store.clear("org.example.fixture", "list"));
        QVERIFY(!store.configure(temp.path() + "/other", {})); QCOMPARE(store.workspaceId(), scope);
        QCOMPARE(bytes(dataPath(directory)), before); QCOMPARE(bytes(directory + "/catalog.json"), catalogue);
        QVERIFY(QFile::setPermissions(directory, permissions));
        const auto outside = temp.path() + "/outside.json"; write(outside, before); QVERIFY(QFile::remove(dataPath(directory))); QVERIFY(QFile::link(outside, dataPath(directory)));
        QCOMPARE(store.generation("org.example.fixture", "list"), 0); QVERIFY(store.resolve(action(store), now()).isEmpty());
        QVERIFY(!store.publish("org.example.fixture", "list", snapshot(2))); QVERIFY(!store.clear("org.example.fixture", "list"));
        QVERIFY(store.configure(temp.path(), {})); QCOMPARE(bytes(outside), before); QVERIFY(QFileInfo(dataPath(directory)).isSymLink());
        WidgetStore linked(temp.path() + "/linked"); QVERIFY(QFile::link(directory, temp.path() + "/linked")); QVERIFY(!linked.configure(temp.path(), providers()));
    }
    void corruptCatalogRepairsWithoutGrantingDeletionAuthority() {
        QTemporaryDir temp; const auto directory = temp.path() + "/widgets"; WidgetStore store(directory);
        QVERIFY(store.configure(temp.path(), providers())); QVERIFY(store.publish("org.example.fixture", "list", snapshot())); const auto before = bytes(dataPath(directory));
        write(directory + "/catalog.json", QByteArray(256 * 1024 + 1, 'x')); QVERIFY(QDir().mkpath(temp.path() + "/other"));
        QVERIFY(store.configure(temp.path() + "/other", providers())); QCOMPARE(bytes(dataPath(directory)), before); QCOMPARE(store.generation("org.example.fixture", "list"), 0);
        QVERIFY(store.publish("org.example.fixture", "list", snapshot())); QVERIFY(store.clear("org.example.fixture", "list"));
        QVERIFY(!QFile::exists(dataPath(directory))); QCOMPARE(store.generation("org.example.fixture", "list"), 0);
    }
    void configurationLimitsAndCachedEnvelopeAreValidated() {
        QTemporaryDir temp; const auto directory = temp.path() + "/widgets"; WidgetStore store(directory); QVERIFY(store.configure(temp.path(), providers())); QVERIFY(store.publish("org.example.fixture", "list", snapshot()));
        const auto before = bytes(directory + "/catalog.json"); auto duplicate = providers(); duplicate.append(duplicate.first()); QVERIFY(!store.configure(temp.path(), duplicate));
        QVariantList tooMany; for (int i = 0; i < 17; ++i) tooMany.append(QVariantMap{{"addonId", "org.example.fixture"}, {"widgetId", QString("widget%1").arg(i)}, {"name", "Fixture"}});
        QVERIFY(!store.configure(temp.path(), tooMany)); QCOMPARE(bytes(directory + "/catalog.json"), before);
        auto cached = QJsonDocument::fromJson(bytes(dataPath(directory))).object().toVariantMap(); cached["workspaceId"] = QString(64, '0');
        write(dataPath(directory), QJsonDocument::fromVariant(cached).toJson()); QCOMPARE(store.generation("org.example.fixture", "list"), 0); QVERIFY(store.resolve(action(store), now()).isEmpty());
        QVERIFY(store.publish("org.example.fixture", "list", snapshot(2))); cached = QJsonDocument::fromJson(bytes(dataPath(directory))).object().toVariantMap(); cached["generation"] = true;
        write(dataPath(directory), QJsonDocument::fromVariant(cached).toJson()); QCOMPARE(store.generation("org.example.fixture", "list"), 0);
        if (!store.available()) { QVERIFY(!store.requestAdd("org.example.fixture", "list")); QVERIFY(!store.error().isEmpty()); }
    }
    void qmlValuesAndUnicodeLimitsUseTheSameContract() {
        QTemporaryDir temp; WidgetStore store(temp.path() + "/widgets"); QVERIFY(store.configure(temp.path(), providers()));
        QJSEngine engine; QQmlEngine::setObjectOwnership(&store, QQmlEngine::CppOwnership); engine.globalObject().setProperty("store", engine.newQObject(&store));
        auto value = snapshot(); auto frames = value.value("timeline").toList(); auto frame = frames.first().toMap(); auto rows = frame.value("rows").toList(); auto row = rows.first().toMap();
        const QString emoji = QString::fromUcs4(U"\U0001f4c5"); row["title"] = emoji.repeated(256); row["detail"] = "First line\nSecond line"; rows[0] = row; frame["rows"] = rows; frames[0] = frame; value["timeline"] = frames;
        engine.globalObject().setProperty("snapshot", engine.toScriptValue(value));
        const auto result = engine.evaluate("store.publish('org.example.fixture','list',snapshot)"); QVERIFY2(result.toBool(), qPrintable(result.toString() + store.error()));
        QCOMPARE(store.generation("org.example.fixture", "list"), 1);
        value["generation"] = 2; frame["at"] = "2026-11-09T00:00:00.000Z"; value["timeline"] = QVariantList{frame}; value["updatedAt"] = "2026-11-09T12:00:00.1Z";
        QVERIFY(store.publish("org.example.fixture", "list", value));
    }
    void sharedSchemaVectors_data() {
        QTest::addColumn<QVariantMap>("value"); QTest::addColumn<bool>("expected");
        QFile file(QFINDTESTDATA("../fixtures/widgets/contract-v1.json")); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto cases = QJsonDocument::fromJson(file.readAll()).object().value("cases").toArray(); QCOMPARE(cases.size(), 47);
        for (const auto &entry : cases) {
            const auto row = entry.toObject(); auto value = row.value("snapshot").toObject().toVariantMap();
            value.remove("workspaceId"); value.remove("addonId"); value.remove("widgetId");
            QTest::newRow(qPrintable(row.value("name").toString())) << value << row.value("schemaValid").toBool();
        }
    }
    void invalidClockPreservesCommittedSnapshot() {
        QTemporaryDir temp; const auto directory = temp.path() + "/widgets"; WidgetStore store(directory);
        QVERIFY(store.configure(temp.path(), providers())); auto value = snapshot();
        auto frames = value.value("timeline").toList(); auto frame = frames.first().toMap();
        frame["clock"] = QVariantMap{{"date", "2026-11-09"}, {"timeZone", "Etc/Unknown_Widget_Clock"}};
        frames[0] = frame; value["timeline"] = frames;
        QVERIFY(store.publish("org.example.fixture", "list", value)); const auto committed = bytes(dataPath(directory));
        frame["clock"] = QVariantMap{{"date", "2026-02-30"}, {"timeZone", "UTC"}};
        frames[0] = frame; value["timeline"] = frames; value["generation"] = 2;
        QVERIFY(!store.publish("org.example.fixture", "list", value));
        QCOMPARE(bytes(dataPath(directory)), committed); QCOMPARE(store.generation("org.example.fixture", "list"), 1);
    }
    void sharedSchemaVectors() {
        QFETCH(QVariantMap, value); QFETCH(bool, expected); QTemporaryDir temp; WidgetStore store(temp.path() + "/widgets");
        QVERIFY(store.configure(temp.path(), providers())); QCOMPARE(store.publish("org.example.fixture", "list", value), expected);
    }
    void catalogueBoundsAndNameLimitsPreserveThePreviousCatalogue() {
        QTemporaryDir temp; const auto directory = temp.path() + "/widgets"; WidgetStore store(directory);
        QVERIFY(store.configure(temp.path(), providers())); const auto before = bytes(directory + "/catalog.json");
        auto provider = providers().first().toMap(); provider["name"] = QString(129, 'x'); QVERIFY(!store.configure(temp.path(), {provider}));
        QVariantList oversized;
        for (int i = 0; i < 1025; ++i) oversized.append(QVariantMap{{"addonId", QString("org.example.a%1").arg(i)}, {"widgetId", "list"}, {"name", "Fixture"}});
        QVERIFY(!store.configure(temp.path(), oversized)); oversized.removeLast();
        const auto longName = QString::fromUcs4(U"\U0001f4c5").repeated(128);
        for (auto &item : oversized) { auto row = item.toMap(); row["name"] = longName; item = row; }
        QVERIFY(!store.configure(temp.path(), oversized)); QCOMPARE(bytes(directory + "/catalog.json"), before);
    }
    void linkedCataloguesAndNonregularCachesAreNeverFollowed() {
        QTemporaryDir temp; const auto directory = temp.path() + "/widgets"; WidgetStore store(directory);
        QVERIFY(store.configure(temp.path(), providers())); QVERIFY(store.publish("org.example.fixture", "list", snapshot()));
        const auto provider = dataPath(directory); const auto original = bytes(provider); QVERIFY(QFile::remove(provider));
        QVERIFY(::mkfifo(QFile::encodeName(provider).constData(), 0600) == 0);
        QCOMPARE(store.generation("org.example.fixture", "list"), 0); QVERIFY(!store.publish("org.example.fixture", "list", snapshot(2))); QVERIFY(!store.clear("org.example.fixture", "list"));
        QVERIFY(QFile::remove(provider)); write(provider, original);
        const auto outside = temp.path() + "/outside.json"; QVERIFY(::link(QFile::encodeName(provider).constData(), QFile::encodeName(outside).constData()) == 0);
        QCOMPARE(store.generation("org.example.fixture", "list"), 0); QVERIFY(!store.publish("org.example.fixture", "list", snapshot(2))); QVERIFY(!store.clear("org.example.fixture", "list"));
        QVERIFY(QFile::remove(outside));
        const auto catalogue = directory + "/catalog.json", copy = temp.path() + "/catalogue-copy.json"; const auto before = bytes(catalogue);
        write(copy, before); QVERIFY(QFile::remove(catalogue)); QVERIFY(QFile::link(copy, catalogue));
        QVERIFY(!store.configure(temp.path(), {})); QCOMPARE(store.generation("org.example.fixture", "list"), 0); QCOMPARE(bytes(copy), before);
        QVERIFY(QFile::remove(catalogue)); write(catalogue, before);
        WidgetStore ancestor(temp.path() + "/alias/another"); QVERIFY(QFile::link(directory, temp.path() + "/alias")); QVERIFY(!ancestor.configure(temp.path(), providers()));
        QVERIFY(!QFile::exists(directory + "/another"));
    }
    void canonicalWorkspaceAndCatalogueAuthorityRejectStaleInstances() {
        QTemporaryDir temp; const auto directory = temp.path() + "/widgets"; WidgetStore first(directory);
        QVERIFY(first.configure(temp.path(), providers())); QVERIFY(first.publish("org.example.fixture", "list", snapshot())); const auto previous = first.workspaceId();
        QVERIFY(first.configure(temp.path() + "/.", providers())); QCOMPARE(first.workspaceId(), previous); QCOMPARE(first.generation("org.example.fixture", "list"), 1);
        QVERIFY(QDir().mkpath(temp.path() + "/other")); WidgetStore second(directory); QVERIFY(second.configure(temp.path() + "/other", providers()));
        QVERIFY(!first.publish("org.example.fixture", "list", snapshot(2))); QCOMPARE(first.generation("org.example.fixture", "list"), 0); QVERIFY(first.resolve(action(first), now()).isEmpty());
        QVERIFY(second.publish("org.example.fixture", "list", snapshot())); QCOMPARE(second.generation("org.example.fixture", "list"), 1);
    }
};
QTEST_GUILESS_MAIN(WidgetTests)
#include "widget_tests.moc"
