#include "addonregistry.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QResource>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QXmlStreamWriter>
#include <QtTest>

class AddonTests : public QObject {
    Q_OBJECT
    QString rcc;
    static QJsonObject manifest(const QString &id = "org.example.counter") {
        return {{"id", id},
                {"name", "Counter"},
                {"version", "1.0.0"},
                {"apiVersion", 1},
                {"page", "Page.qml"},
                {"settings",
                 QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}, {"entry", "Settings.qml"}}}}};
    }
    void package(const QString &path, const QJsonObject &data,
                 const QByteArray &page = "not valid QML; discovery must not execute this") {
        QTemporaryDir source;
        const QMap<QString, QByteArray> files{{"manifest.json", QJsonDocument(data).toJson()},
                                              {"Page.qml", page},
                                              {"Settings.qml", "import QtQuick\nItem {}"},
                                              {"Runtime.qml", "import QtQml\nQtObject {}"},
                                              {"Lifecycle.qml", "import QtQml\nQtObject {}"},
                                              {"Integration.qml", "import QtQml\nQtObject {}"}};
        QFile qrc(source.filePath("package.qrc"));
        QVERIFY(qrc.open(QIODevice::WriteOnly));
        QXmlStreamWriter xml(&qrc);
        xml.writeStartElement("RCC");
        xml.writeStartElement("qresource");
        xml.writeAttribute("prefix", "/");
        for (auto i = files.cbegin(); i != files.cend(); ++i) {
            QFile file(source.filePath(i.key()));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(i.value()), i.value().size());
            file.close();
            xml.writeStartElement("file");
            xml.writeAttribute("alias", i.key());
            xml.writeCharacters(file.fileName());
            xml.writeEndElement();
        }
        xml.writeEndElement();
        xml.writeEndElement();
        qrc.close();
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QProcess compile;
        compile.start(rcc, {"--binary", "--format-version", "3", "--output", path, qrc.fileName()});
        QVERIFY(compile.waitForFinished(10000));
        QVERIFY2(compile.exitCode() == 0, compile.readAllStandardError().constData());
    }
    static QVariantMap row(const AddonRegistry &registry, const QString &id) {
        for (const auto &value : registry.addons())
            if (value.toMap().value("id") == id)
                return value.toMap();
        return {};
    }
  private slots:
    void initTestCase() {
        if (QFileInfo::exists("/usr/lib/qt6/rcc"))
            rcc = "/usr/lib/qt6/rcc";
        if (rcc.isEmpty())
            rcc = QStandardPaths::findExecutable("rcc6");
        if (rcc.isEmpty())
            rcc = QStandardPaths::findExecutable("rcc");
        QVERIFY2(!rcc.isEmpty(),
                 "Qt rcc is required to build the application's QML resources and test packages.");
    }
    void init() {
        QSettings().clear();
        QSettings().setValue("app/setupVersion", 1);
        QSettings().sync();
    }
    void unreadableOwnershipPreventsPackagePublication() {
        QTemporaryDir root;
        const auto source = root.filePath("source.piext");
        package(source, manifest());
        const auto state = root.filePath("data/.registry");
        QVERIFY(QDir().mkpath(state));
        QFile ownership(state + "/registry.json");
        QVERIFY(ownership.open(QIODevice::WriteOnly));
        QVERIFY(ownership.write("damaged ownership record") > 0);
        ownership.close();
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"), false);
        QVERIFY(!registry.install(QUrl::fromLocalFile(source)));
        QVERIFY(registry.error().contains("ownership"));
        QVERIFY(!QFileInfo::exists(root.filePath("installed/org.example.counter/org.example.counter.piext")));
        QVERIFY(registry.addons().isEmpty());
    }
    void portableSettingsRejectOversizedValuesBeforePublishing() {
        QTemporaryDir root;
        const auto source = root.filePath("source.piext");
        auto data = manifest();
        data["portableSettings"] = QJsonArray{"label"};
        package(source, data);
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"), false);
        QVERIFY(registry.install(QUrl::fromLocalFile(source)));
        const QString id = "org.example.counter";
        QVERIFY(registry.setSetting(id, "label", "existing"));
        QSignalSpy changed(&registry, &AddonRegistry::changed);
        QVERIFY(registry.setSetting(id, "label", "existing"));
        QVERIFY(changed.isEmpty());
        QVERIFY(!registry.setSetting(id, "label", QString(1024 * 1024 + 1, 'x')));
        QCOMPARE(registry.setting(id, "label").toString(), QString("existing"));
    }
    void configuredRestoreVerifiesIdentityVersionAndDigestBeforeInstallation() {
        QTemporaryDir root;
        const QString id = "org.example.counter";
        const auto source = root.filePath("source/counter.piext");
        package(source, manifest(id));
        QFile packageFile(source);
        QVERIFY(packageFile.open(QIODevice::ReadOnly));
        const auto digest = QString::fromLatin1(QCryptographicHash::hash(packageFile.readAll(), QCryptographicHash::Sha256).toHex());
        const auto url = QUrl::fromLocalFile(source);
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"), false);
        QVERIFY(!registry.installConfigured(url, "org.example.other", "", ""));
        QVERIFY(!registry.installConfigured(url, id, "1.2.3", digest));
        QVERIFY(!registry.installConfigured(url, id, "", QString(64, '0')));
        QVERIFY(registry.addons().isEmpty());
        QVERIFY(!registry.downloadConfigured(QUrl("http://example.invalid/release.piext"), id, "", ""));
        QVERIFY(!registry.downloadConfigured(QUrl("https://example.invalid/repository"), id, "", ""));
        QSettings().setValue("addons/enabled/" + id, true);
        QSettings().sync();
        QVERIFY(registry.installConfigured(url, id, "1.0.0", digest.toUpper()));
        QCOMPARE(row(registry, id).value("sha256").toString(), digest);
        QVERIFY(!registry.isEnabled(id));
        QVERIFY(!registry.installConfigured(url, id, "1.0.0", digest));
        QVERIFY(registry.setEnabled(id, true));
        QVERIFY(registry.isEnabled(id));
    }
    void restoreRemovedBuiltinUsesPackagedCopyWithoutNetwork() {
        QTemporaryDir root;
        package(root.filePath("builtins/files.piext"), manifest("files"));
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"), false);
        const auto digest = row(registry, "files")["sha256"].toString();
        QVERIFY(!digest.isEmpty());
        QVERIFY(registry.setEnabled("files", false));
        QVERIFY2(registry.finalizeRemoval("files", true), qPrintable(registry.error()));
        QVERIFY(row(registry, "files").isEmpty());
        QVERIFY2(registry.installConfiguredBuiltin("files", "1.0.0", digest), qPrintable(registry.error()));
        QCOMPARE(row(registry,"files").value("sha256").toString(),digest);
        QVERIFY(!registry.isEnabled("files"));
    }
    void installedStagingIsConsumedAndRemovedAddonsStayRemoved() {
        QTemporaryDir root;
        const auto source = root.filePath("builtins/files.piext");
        const auto legacy = root.filePath("installed/files.piext");
        const auto installed = root.filePath("installed/files/files.piext");
        package(source, manifest("files"));
        package(installed, manifest("files"));
        QVERIFY(QFile::copy(installed, legacy));
        QFile marker(root.filePath("builtins/.install-packages"));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.close();
        const auto state = root.filePath("state");
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), state, false);
        QVERIFY2(registry.error().isEmpty(), qPrintable(registry.error()));
        QVERIFY(!QFileInfo::exists(legacy));
        QVERIFY(!QFileInfo::exists(source));
        QVERIFY(QFileInfo::exists(installed));
        QVERIFY(!QFileInfo::exists(state + "/cache"));
        QVERIFY(registry.setEnabled("files", false));
        QVERIFY2(registry.finalizeRemoval("files", true), qPrintable(registry.error()));
        package(source, manifest("files"));
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.close();
        AddonRegistry upgraded(root.filePath("builtins"), root.filePath("installed"), state, false);
        QVERIFY2(upgraded.error().isEmpty(), qPrintable(upgraded.error()));
        QVERIFY(!QFileInfo::exists(installed));
        QVERIFY(!QFileInfo::exists(source));
    }
    void packagesAreDiscoveredWithoutQmlExecutionAndChoicesPersist() {
        QTemporaryDir root;
        package(root.filePath("builtins/files.piext"), manifest("files"));
        package(root.filePath("installed/counter.piext"), manifest());
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        QVERIFY(registry.isEnabled("files"));
        QVERIFY(!registry.isEnabled("org.example.counter"));
        const auto addon = row(registry, "org.example.counter");
        QVERIFY(addon.value("error").toString().isEmpty());
        QCOMPARE(addon.value("page").toUrl().scheme(), QString("qrc"));
        QCOMPARE(addon.value("settings").toList().size(), 1);
        QFile loaded(':' + addon.value("page").toUrl().path());
        QVERIFY(loaded.open(QIODevice::ReadOnly));
        QVERIFY(loaded.readAll().startsWith("not valid QML"));
        QVERIFY(registry.setEnabled("org.example.counter", true));
        QVERIFY(registry.lifecycleOwners()
                    .last()
                    .toMap()
                    .value("ownedPaths")
                    .toMap()
                    .value("addonData")
                    .toList()
                    .contains(QString("addons/org.example.counter/data")));
        AddonRegistry next(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                           false);
        QVERIFY(next.isEnabled("org.example.counter"));
        QVERIFY(next.setEnabled("files", false));
        QVERIFY(!next.isEnabled("files"));
        QCOMPARE(QSettings().value("app/setupVersion").toInt(), 1);
    }
    void embeddedBuiltinPackagesLoadThroughAnImmutableSnapshot() {
        QTemporaryDir root;
        const auto inner = root.filePath("files.piext");
        package(inner, manifest("files"));
        QFile qrc(root.filePath("embedded.qrc"));
        QVERIFY(qrc.open(QIODevice::WriteOnly));
        QXmlStreamWriter xml(&qrc);
        xml.writeStartElement("RCC");
        xml.writeStartElement("qresource");
        xml.writeAttribute("prefix", "/embedded-pilon-test");
        xml.writeStartElement("file");
        xml.writeAttribute("alias", "files.piext");
        xml.writeCharacters(inner);
        xml.writeEndElement(); xml.writeEndElement(); xml.writeEndElement();
        qrc.close();
        const auto outer = root.filePath("embedded.rcc");
        QProcess compile;
        compile.start(rcc, {"--binary", "--format-version", "3", "--output", outer, qrc.fileName()});
        QVERIFY(compile.waitForFinished(10000));
        QCOMPARE(compile.exitCode(), 0);
        QVERIFY(QResource::registerResource(outer));
        const auto unmount = qScopeGuard([&] { QResource::unregisterResource(outer); });
        AddonRegistry registry(":/embedded-pilon-test", root.filePath("installed"), root.filePath("data/.registry"), false);
        QVERIFY2(registry.error().isEmpty(), qPrintable(registry.error()));
        QVERIFY(registry.isEnabled("files"));
        QVERIFY(QFileInfo::exists(root.filePath("installed/files/files.piext")));
        QFile page(':' + row(registry, "files").value("page").toUrl().path());
        QVERIFY(page.open(QIODevice::ReadOnly));
        QVERIFY(page.readAll().startsWith("not valid QML"));
    }
    void sidebarOrderPersistsAndKeepsDisabledPagesAndNewAddons() {
        QTemporaryDir root;
        package(root.filePath("builtins/files.piext"), manifest("files"));
        package(root.filePath("builtins/notes.piext"), manifest("notes"));
        package(root.filePath("installed/counter.piext"), manifest());
        const auto ids = [](const QVariantList &rows) {
            QStringList result;
            for (const auto &value : rows)
                result << value.toMap().value("id").toString();
            return result;
        };
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"), false);
        const auto original = ids(registry.addons());
        QVERIFY(registry.setSidebarOrder({"org.example.counter", "notes", "files"}));
        QVERIFY(registry.setEnabled("notes", false));
        AddonRegistry next(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"), false);
        QCOMPARE(ids(next.sidebarAddons()), (QStringList{"org.example.counter", "notes", "files"}));
        QCOMPARE(ids(next.addons()), original);
        QVERIFY(next.setEnabled("notes", true));
        QCOMPARE(ids(next.sidebarAddons()), (QStringList{"org.example.counter", "notes", "files"}));
        package(root.filePath("installed/new.piext"), manifest("org.example.new"));
        next.refresh();
        QCOMPARE(ids(next.sidebarAddons()), (QStringList{"org.example.counter", "notes", "files", "org.example.new"}));
        QVERIFY(next.setSidebarOrder({"files", "org.example.counter"}));
        QCOMPARE(ids(next.sidebarAddons()), (QStringList{"files", "org.example.counter", "notes", "org.example.new"}));
        QVERIFY(!next.setSidebarOrder({"files", "files"}));
        QVERIFY(!next.setSidebarOrder({"org.example.missing"}));
        QCOMPARE(ids(next.sidebarAddons()), (QStringList{"files", "org.example.counter", "notes", "org.example.new"}));
        QSettings().setValue("addons/sidebarOrder", QStringList{"org.example.missing", "notes", "notes"});
        QCOMPARE(ids(next.sidebarAddons()), (QStringList{"notes", "files", "org.example.counter", "org.example.new"}));
    }
    void sidebarOrderWriteFailureKeepsPreviousOrder() {
        QTemporaryDir root;
        package(root.filePath("builtins/files.piext"), manifest("files"));
        package(root.filePath("builtins/notes.piext"), manifest("notes"));
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"), false);
        QVERIFY(registry.setSidebarOrder({"notes", "files"}));
        const auto folder = QFileInfo(QSettings().fileName()).absolutePath();
        const auto permissions = QFileInfo(folder).permissions();
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, permissions); });
        QVERIFY(!registry.setSidebarOrder({"files", "notes"}));
        QCOMPARE(registry.sidebarAddons().first().toMap().value("id").toString(), QString("notes"));
    }
    void pageVisibilityPersistsWithoutDisablingOrReorderingAddons() {
        QTemporaryDir root;
        package(root.filePath("builtins/files.piext"), manifest("files"));
        package(root.filePath("builtins/notes.piext"), manifest("notes"));
        auto settingsOnly = manifest("org.example.settings");
        settingsOnly.remove("page");
        package(root.filePath("installed/settings.piext"), settingsOnly);
        const auto page = [](const AddonRegistry &registry, const QString &id) {
            for (const auto &value : registry.sidebarAddons())
                if (value.toMap().value("id").toString() == id)
                    return value.toMap();
            return QVariantMap{};
        };
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"), false);
        QVERIFY(registry.setSidebarOrder({"notes", "files"}));
        QVERIFY(registry.setSetting("notes", "marker", 42));
        QVERIFY(page(registry, "notes").value("sidebarVisible").toBool());
        QVERIFY(registry.setPageVisible("notes", false));
        QVERIFY(!page(registry, "notes").value("sidebarVisible").toBool());
        QVERIFY(registry.isEnabled("notes"));
        QCOMPARE(registry.setting("notes", "marker", 0).toInt(), 42);
        QCOMPARE(registry.sidebarAddons().first().toMap().value("id").toString(), QString("notes"));
        QVERIFY(!registry.setPageVisible("org.example.missing", false));
        QVERIFY(!registry.setPageVisible("org.example.settings", false));
        QCOMPARE(QSettings().value("addons/hiddenPages").toStringList(), QStringList{"notes"});

        AddonRegistry next(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"), false);
        QVERIFY(next.isEnabled("notes"));
        QVERIFY(!page(next, "notes").value("sidebarVisible").toBool());
        QVERIFY(next.setEnabled("notes", false));
        QVERIFY(!page(next, "notes").value("sidebarVisible").toBool());
        QVERIFY(next.setEnabled("notes", true));
        QVERIFY(!page(next, "notes").value("sidebarVisible").toBool());
        QVERIFY(next.setSidebarOrder({"files", "notes"}));
        QCOMPARE(next.sidebarAddons().last().toMap().value("id").toString(), QString("notes"));
        QVERIFY(next.setPageVisible("notes", true));
        QVERIFY(page(next, "notes").value("sidebarVisible").toBool());
        QVERIFY(next.isEnabled("notes"));
        QCOMPARE(next.setting("notes", "marker", 0).toInt(), 42);
        QCOMPARE(next.sidebarAddons().last().toMap().value("id").toString(), QString("notes"));
    }
    void failedPageVisibilityWriteKeepsPreviousChoice_data() {
        QTest::addColumn<bool>("initiallyHidden");
        QTest::newRow("hide") << false;
        QTest::newRow("show") << true;
    }
    void failedPageVisibilityWriteKeepsPreviousChoice() {
        QFETCH(bool, initiallyHidden);
        QTemporaryDir root;
        package(root.filePath("builtins/notes.piext"), manifest("notes"));
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"), false);
        if (initiallyHidden)
            QVERIFY(registry.setPageVisible("notes", false));
        const auto folder = QFileInfo(QSettings().fileName()).absolutePath();
        const auto permissions = QFileInfo(folder).permissions();
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, permissions); });
        QVERIFY(!registry.setPageVisible("notes", initiallyHidden));
        QCOMPARE(registry.sidebarAddons().first().toMap().value("sidebarVisible").toBool(), !initiallyHidden);
        QVERIFY(registry.isEnabled("notes"));
    }
    void descriptionsAreOptionalAndCompilerValidationMatchesInstaller_data() {
        QTest::addColumn<QJsonValue>("description");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("absent") << QJsonValue(QJsonValue::Undefined) << true;
        QTest::newRow("plain") << QJsonValue("Manage <b>daily</b> tasks.") << true;
        QTest::newRow("limit") << QJsonValue(QString(200, 'a')) << true;
        QTest::newRow("unicode-limit") << QJsonValue(QString::fromUcs4(U"📝").repeated(200)) << true;
        QTest::newRow("unicode-too-long") << QJsonValue(QString::fromUcs4(U"📝").repeated(201)) << false;
        QTest::newRow("number") << QJsonValue(12) << false;
        QTest::newRow("null") << QJsonValue(QJsonValue::Null) << false;
        QTest::newRow("blank") << QJsonValue(" ") << false;
        QTest::newRow("too-long") << QJsonValue(QString(201, 'a')) << false;
        QTest::newRow("control") << QJsonValue("Two\nlines") << false;
    }
    void descriptionsAreOptionalAndCompilerValidationMatchesInstaller() {
        QFETCH(QJsonValue, description);
        QFETCH(bool, accepted);
        QTemporaryDir root;
        auto data = manifest();
        if (!description.isUndefined())
            data["description"] = description;
        package(root.filePath("installed/input.piext"), data);
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        const auto item = row(registry, "org.example.counter");
        QCOMPARE(item.value("error").toString().isEmpty(), accepted);
        if (accepted)
            QCOMPARE(item.value("description").toString(), description.toString());
        const auto source = root.filePath("source");
        QVERIFY(QDir().mkpath(source));
        for (const auto &name : {QString("Page.qml"), QString("Settings.qml"), QString("manifest.json")}) {
            QFile file(QDir(source).filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(name == "manifest.json" ? QJsonDocument(data).toJson()
                                               : QByteArray("import QtQuick\nItem {}"));
        }
        const auto project =
            QDir(QStringLiteral(PILON_SOURCE_ROOT)).absolutePath();
        QProcess compile;
        compile.start(QStandardPaths::findExecutable("python3"),
                      {project + "/src/scripts/compile-addon.py", source, "--output",
                       root.filePath("compiled.piext"), "--rcc", rcc});
        QVERIFY(compile.waitForFinished(10000));
        QCOMPARE(compile.exitCode() == 0, accepted);
        if (accepted) {
            AddonRegistry compiled(root.filePath("builtins"), root.filePath("compiled-installed"),
                                   root.filePath("compiled-data"), false);
            QVERIFY(compiled.install(QUrl::fromLocalFile(root.filePath("compiled.piext"))));
            QCOMPARE(row(compiled, "org.example.counter").value("description").toString(),
                     description.toString());
        }
    }
    void widgetManifestValidationMatchesCompiler_data() {
        QTest::addColumn<QJsonObject>("data");
        QTest::addColumn<bool>("accepted");
        const auto add = [](const char *name, const QJsonObject &data, bool accepted) {
            QTest::newRow(name) << data << accepted;
        };
        const QJsonObject widget{{"id", "agenda"}, {"name", "Agenda"}};
        auto valid = manifest();
        valid["runtime"] = "Runtime.qml";
        valid["widgets"] = QJsonArray{widget};
        add("legacy-no-widgets", manifest(), true);
        auto empty = manifest();
        empty["widgets"] = QJsonArray{};
        add("empty-without-runtime", empty, true);
        add("one-widget", valid, true);
        auto only = valid;
        only.remove("page"); only.remove("settings");
        add("widget-runtime-without-page", only, true);
        auto noRuntime = valid;
        noRuntime.remove("runtime");
        add("widget-requires-runtime", noRuntime, false);
        const auto widgets = [&](const char *name, const QJsonValue &value, bool accepted) {
            auto data = valid; data["widgets"] = value; add(name, data, accepted);
        };
        widgets("not-array", QJsonObject{}, false);
        widgets("null-array", QJsonValue(QJsonValue::Null), false);
        widgets("boolean-array", true, false);
        widgets("nonobject-widget", QJsonArray{true}, false);
        widgets("duplicate-id", QJsonArray{widget, widget}, false);
        widgets("missing-id", QJsonArray{QJsonObject{{"name", "Agenda"}}}, false);
        widgets("missing-name", QJsonArray{QJsonObject{{"id", "agenda"}}}, false);
        widgets("unknown-entry", QJsonArray{QJsonObject{{"id", "agenda"}, {"name", "Agenda"}, {"entry", "Runtime.qml"}}}, false);
        const auto field = [&](const char *name, const QString &key, const QJsonValue &value, bool accepted) {
            auto changed = widget; changed[key] = value; widgets(name, QJsonArray{changed}, accepted);
        };
        field("uppercase-id", "id", "Agenda", false);
        field("namespaced-local-id", "id", "org.example.agenda", false);
        field("traversal-id", "id", "../agenda", false);
        field("numeric-id", "id", 1, false);
        field("id-limit", "id", QString(64, 'a'), true);
        field("id-too-long", "id", QString(65, 'a'), false);
        field("blank-name", "name", " \t", false);
        field("control-name", "name", "Two\nlines", false);
        field("delete-control-name", "name", QString("Name") + QChar(0x7f), false);
        field("numeric-name", "name", 2, false);
        field("name-limit", "name", QString(128, 'a'), true);
        field("name-too-long", "name", QString(129, 'a'), false);
        field("unicode-name-limit", "name", QString::fromUcs4(U"📝").repeated(128), true);
        field("unicode-name-too-long", "name", QString::fromUcs4(U"📝").repeated(129), false);
        QJsonArray maximum;
        for (int index = 0; index < 16; ++index)
            maximum.append(QJsonObject{{"id", QString("widget-%1").arg(index)}, {"name", "Widget"}});
        widgets("sixteen-widgets", maximum, true);
        maximum.append(QJsonObject{{"id", "seventeenth"}, {"name", "Widget"}});
        widgets("too-many-widgets", maximum, false);
    }
    void widgetManifestValidationMatchesCompiler() {
        QFETCH(QJsonObject, data);
        QFETCH(bool, accepted);
        QTemporaryDir root;
        package(root.filePath("installed/input.piext"), data);
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"), false);
        const auto descriptor = row(registry, "org.example.counter");
        QCOMPARE(descriptor.value("error").toString().isEmpty(), accepted);
        if (accepted) {
            QCOMPARE(descriptor.value("widgets").metaType().id(), QMetaType::QVariantList);
            QCOMPARE(descriptor.value("widgets").toList(), data.value("widgets").toArray().toVariantList());
            QVERIFY(registry.setEnabled("org.example.counter", true));
            if (!data.contains("page")) QVERIFY(registry.sidebarAddons().isEmpty());
        } else QVERIFY(!registry.isEnabled("org.example.counter"));
        const auto source = root.filePath("source");
        QVERIFY(QDir().mkpath(source));
        for (const auto &name : {QString("Page.qml"), QString("Settings.qml"), QString("Runtime.qml"), QString("manifest.json")}) {
            QFile file(QDir(source).filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(name == "manifest.json" ? QJsonDocument(data).toJson() : QByteArray("import QtQml\nQtObject {}"));
        }
        const auto project = QDir(QStringLiteral(PILON_SOURCE_ROOT)).absolutePath();
        QProcess compile;
        compile.start(QStandardPaths::findExecutable("python3"), {project + "/src/scripts/compile-addon.py", source,
            "--output", root.filePath("compiled.piext"), "--rcc", rcc});
        QVERIFY(compile.waitForFinished(10000));
        QCOMPARE(compile.exitCode() == 0, accepted);
        if (accepted) {
            AddonRegistry compiled(root.filePath("builtins"), root.filePath("compiled-installed"), root.filePath("compiled-data"), false);
            QVERIFY(compiled.install(QUrl::fromLocalFile(root.filePath("compiled.piext"))));
            QCOMPARE(row(compiled, "org.example.counter").value("widgets").toList(), data.value("widgets").toArray().toVariantList());
        }
    }
    void invalidManifests_data() {
        QTest::addColumn<QJsonObject>("data");
        const auto add = [](const char *name, const QString &key, const QJsonValue &value) {
            auto data = manifest();
            data[key] = value;
            QTest::newRow(name) << data;
        };
        add("traversal-id", "id", "../escape");
        add("unnamespaced-id", "id", "counter");
        add("unsupported-api", "apiVersion", 2);
        add("boolean-api", "apiVersion", true);
        add("empty-name", "name", "");
        add("absolute-page", "page", "/tmp/Page.qml");
        add("remote-page", "page", "https://example.test/Page.qml");
        add("traversal-page", "page", "../Page.qml");
        add("backslash-page", "page", "nested\\Page.qml");
        add("missing-page", "page", "Missing.qml");
        add("wrong-page-type", "page", true);
        add("settings-not-array", "settings", "Settings.qml");
        add("runtime-missing", "runtime", "Missing.qml");
        add("start-no-runtime", "startOnEnable", true);
        add("start-string", "startOnEnable", "true");
        add("settings-duplicate", "settings",
            QJsonArray{QJsonObject{{"id", "general"}, {"name", "One"}, {"entry", "Settings.qml"}},
                       QJsonObject{{"id", "general"}, {"name", "Two"}, {"entry", "Settings.qml"}}});
        add("settings-escaping", "settings",
            QJsonArray{QJsonObject{{"id", "general"}, {"name", "General"}, {"entry", "../Settings.qml"}}});
    }
    void dependencyAndIntegrationManifestValidationMatchesCompiler_data() {
        QTest::addColumn<QJsonObject>("data");
        QTest::addColumn<bool>("accepted");
        const auto add = [](const char *name, const QJsonObject &data, bool accepted) {
            QTest::newRow(name) << data << accepted;
        };
        auto valid = manifest();
        valid.remove("page");
        valid["dependencies"] = QJsonArray{
            QJsonObject{{"id", "org.example.provider"}, {"minVersion", "1.2.0"}, {"maxVersion", "2.0.0"}}};
        valid["extensionPoints"] = QJsonArray{QJsonObject{{"id", "folder-actions"}, {"apiVersion", 1}}};
        valid["integrations"] = QJsonArray{QJsonObject{{"id", "files-action"},
                                                       {"name", "Folder actions"},
                                                       {"target", "files"},
                                                       {"minVersion", "1.0.0"},
                                                       {"point", "folder-actions"},
                                                       {"apiVersion", 1},
                                                       {"entry", "Integration.qml"}}};
        valid["lifecycle"] = "Lifecycle.qml";
        add("complete-contract", valid, true);
        auto data = manifest();
        data.remove("page");
        data.remove("settings");
        data["lifecycle"] = "Lifecycle.qml";
        add("lifecycle-only", data, true);
        data.remove("lifecycle");
        add("no-entry", data, false);
        data = manifest("devices");
        add("shipped-devices", data, true);
        const auto dependency = [&](const char *name, const QJsonObject &value, bool accepted) {
            auto item = manifest();
            item["dependencies"] = QJsonArray{value};
            add(name, item, accepted);
        };
        dependency("minimum-only", {{"id", "org.example.provider"}, {"minVersion", "1.10.0"}}, true);
        dependency("self-dependency", {{"id", "org.example.counter"}, {"minVersion", "1.0.0"}}, false);
        dependency("leading-zero", {{"id", "files"}, {"minVersion", "01.0.0"}}, false);
        dependency("range-syntax", {{"id", "files"}, {"minVersion", "^1.0.0"}}, false);
        dependency("prerelease", {{"id", "files"}, {"minVersion", "1.0.0-beta"}}, false);
        dependency("equal-bounds", {{"id", "files"}, {"minVersion", "2.0.0"}, {"maxVersion", "2.0.0"}},
                   false);
        dependency("reverse-bounds", {{"id", "files"}, {"minVersion", "2.0.0"}, {"maxVersion", "1.0.0"}},
                   false);
        dependency("unknown-field", {{"id", "files"}, {"minVersion", "1.0.0"}, {"optional", true}}, false);
        dependency("oversized-component", {{"id", "files"}, {"minVersion", "999999999999999999.0.0"}}, false);
        data = valid;
        data["lifecycle"] = "../Lifecycle.qml";
        add("escaping-lifecycle", data, false);
        data = valid;
        auto points = valid["extensionPoints"].toArray();
        points.append(points.first());
        data["extensionPoints"] = points;
        add("duplicate-point", data, false);
        data = valid;
        auto entries = valid["integrations"].toArray();
        auto entry = entries.first().toObject();
        entry["apiVersion"] = true;
        entries[0] = entry;
        data["integrations"] = entries;
        add("boolean-integration-api", data, false);
        data = valid;
        entries = valid["integrations"].toArray();
        entry = entries.first().toObject();
        entry["entry"] = "Missing.qml";
        entries[0] = entry;
        data["integrations"] = entries;
        add("missing-integration-entry", data, false);
        data = valid;
        entries = valid["integrations"].toArray();
        entries.append(entries.first());
        data["integrations"] = entries;
        add("duplicate-integration", data, false);
        const auto integrationDefault = [&](const char *name, const QJsonValue &value, bool accepted) {
            auto item = valid;
            auto integrations = item["integrations"].toArray();
            auto integration = integrations.first().toObject();
            integration["enabledByDefault"] = value;
            integrations[0] = integration;
            item["integrations"] = integrations;
            add(name, item, accepted);
        };
        integrationDefault("integration-default-on", true, true);
        integrationDefault("integration-default-off", false, true);
        integrationDefault("integration-default-string", "true", false);
        integrationDefault("integration-default-number", 1, false);
        integrationDefault("integration-default-null", QJsonValue(QJsonValue::Null), false);
    }
    void dependencyAndIntegrationManifestValidationMatchesCompiler() {
        QFETCH(QJsonObject, data);
        QFETCH(bool, accepted);
        QTemporaryDir root;
        package(root.filePath("installed/input.piext"), data);
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        const auto item = registry.addons().last().toMap();
        QCOMPARE(item.value("error").toString().isEmpty(), accepted);
        const auto source = root.filePath("source");
        QVERIFY(QDir().mkpath(source));
        for (const auto &name :
             {QString("Page.qml"), QString("Settings.qml"), QString("Runtime.qml"), QString("Lifecycle.qml"),
              QString("Integration.qml"), QString("manifest.json")}) {
            QFile file(QDir(source).filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(name == "manifest.json" ? QJsonDocument(data).toJson()
                                               : QByteArray("import QtQuick\nItem {}"));
        }
        const auto project =
            QDir(QStringLiteral(PILON_SOURCE_ROOT)).absolutePath();
        QProcess compile;
        compile.start(QStandardPaths::findExecutable("python3"),
                      {project + "/src/scripts/compile-addon.py", source, "--output",
                       root.filePath("compiled.piext"), "--rcc", rcc});
        QVERIFY(compile.waitForFinished(10000));
        QCOMPARE(compile.exitCode() == 0, accepted);
        if (accepted) {
            QCOMPARE(item.value("lifecycle").toUrl().isEmpty(), !data.contains("lifecycle"));
            QCOMPARE(item.value("dependencies").toList().size(), data["dependencies"].toArray().size());
        }
    }
    void invalidManifests() {
        QFETCH(QJsonObject, data);
        QTemporaryDir root;
        package(root.filePath("installed/invalid.piext"), data);
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        QCOMPARE(registry.addons().size(), 1);
        QVERIFY(!registry.addons().last().toMap().value("error").toString().isEmpty());
        QVERIFY(!registry.addons().last().toMap().value("enabled").toBool());
    }
    void enablingDependenciesIsAtomicAndNumericVersionsAreCompared() {
        QTemporaryDir root;
        auto base = manifest("org.example.base");
        base["version"] = "1.10.0";
        auto left = manifest("org.example.left");
        left["dependencies"] = QJsonArray{
            QJsonObject{{"id", "org.example.base"}, {"minVersion", "1.9.0"}, {"maxVersion", "2.0.0"}}};
        auto right = manifest("org.example.right");
        right["dependencies"] = left["dependencies"];
        auto top = manifest("org.example.top");
        top["dependencies"] = QJsonArray{QJsonObject{{"id", "org.example.left"}, {"minVersion", "1.0.0"}},
                                         QJsonObject{{"id", "org.example.right"}, {"minVersion", "1.0.0"}}};
        for (const auto &data : {base, left, right, top})
            package(root.filePath("installed/" + data["id"].toString() + ".piext"), data);
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        QVERIFY(registry.setEnabled("org.example.top", true));
        QCOMPARE(
            registry.requiredOrder("org.example.top"),
            (QStringList{"org.example.base", "org.example.left", "org.example.right", "org.example.top"}));
        for (const auto &id :
             {"org.example.base", "org.example.left", "org.example.right", "org.example.top"})
            QVERIFY(registry.isEnabled(id));
        QVERIFY(!registry.setEnabled("org.example.base", false));
        QVERIFY(registry.error().contains("org.example.top"));
        const auto dependents = registry.dependents("org.example.base");
        QCOMPARE(dependents.size(), 3);
        QCOMPARE(dependents.first(), QString("org.example.top"));
        QVERIFY(!registry.disableWithDependents("org.example.base", {"org.example.top"}));
        for (const auto &id : dependents) QVERIFY(registry.isEnabled(id));
        QSignalSpy committed(&registry, &AddonRegistry::changed);
        QVERIFY(registry.disableWithDependents("org.example.base", dependents));
        QCOMPARE(committed.count(), 1);
        for (const auto &id : dependents) QVERIFY(!registry.isEnabled(id));
        QVERIFY(!registry.isEnabled("org.example.base"));
        const auto folder = QFileInfo(QSettings().fileName()).absolutePath();
        const auto permissions = QFileInfo(folder).permissions();
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, permissions); });
        QVERIFY(!registry.setEnabled("org.example.top", true));
        for (const auto &id :
             {"org.example.base", "org.example.left", "org.example.right", "org.example.top"})
            QVERIFY(!registry.isEnabled(id));
    }
    void unavailableDependenciesBlockWithoutChangingSavedIntent_data() {
        QTest::addColumn<QString>("providerVersion");
        QTest::addColumn<QString>("maximum");
        QTest::addColumn<bool>("missing");
        QTest::addColumn<bool>("cycle");
        QTest::newRow("missing") << QString("1.0.0") << QString() << true << false;
        QTest::newRow("below-minimum") << QString("1.2.0") << QString() << false << false;
        QTest::newRow("exclusive-maximum") << QString("2.0.0") << QString("2.0.0") << false << false;
        QTest::newRow("legacy-nonnumeric-provider") << QString("release") << QString() << false << false;
        QTest::newRow("cycle") << QString("1.10.0") << QString() << false << true;
    }
    void unavailableDependenciesBlockWithoutChangingSavedIntent() {
        QFETCH(QString, providerVersion);
        QFETCH(QString, maximum);
        QFETCH(bool, missing);
        QFETCH(bool, cycle);
        QTemporaryDir root;
        auto consumer = manifest();
        QJsonObject dependency{{"id", "org.example.provider"}, {"minVersion", "1.9.0"}};
        if (!maximum.isEmpty())
            dependency["maxVersion"] = maximum;
        consumer["dependencies"] = QJsonArray{dependency};
        package(root.filePath("installed/consumer.piext"), consumer);
        auto provider = manifest("org.example.provider");
        provider["version"] = providerVersion;
        if (cycle)
            provider["dependencies"] =
                QJsonArray{QJsonObject{{"id", "org.example.counter"}, {"minVersion", "1.0.0"}}};
        if (!missing)
            package(root.filePath("installed/provider.piext"), provider);
        QSettings().setValue("addons/enabled/org.example.counter", true);
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        QVERIFY(!registry.isEnabled("org.example.counter"));
        QVERIFY(row(registry, "org.example.counter").value("configuredEnabled").toBool());
        QVERIFY(!row(registry, "org.example.counter").value("blockedReason").toString().isEmpty());
        QVERIFY(!registry.setEnabled("org.example.counter", true));
        QVERIFY(!QSettings().contains("addons/enabled/org.example.provider"));
        if (cycle)
            QVERIFY(registry.error().contains("org.example.counter") &&
                    registry.error().contains("org.example.provider"));
    }
    void optionalIntegrationsNeverEnableOrDisableIndependentAddons() {
        QTemporaryDir root;
        auto files = manifest("files");
        files["extensionPoints"] = QJsonArray{QJsonObject{{"id", "folder-actions"}, {"apiVersion", 1}}};
        auto contributor = manifest("org.example.contributor");
        contributor["integrations"] = QJsonArray{QJsonObject{{"id", "files-action"},
                                                     {"name", "Folder actions"},
                                                     {"target", "files"},
                                                     {"minVersion", "1.0.0"},
                                                     {"point", "folder-actions"},
                                                     {"apiVersion", 1},
                                                     {"entry", "Integration.qml"}}};
        package(root.filePath("installed/files/files.piext"), files);
        package(root.filePath("installed/org.example.contributor/org.example.contributor.piext"), contributor);
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        QVERIFY(registry.setEnabled("org.example.contributor", true));
        QVERIFY(registry.isEnabled("org.example.contributor"));
        QVERIFY(!registry.isEnabled("files"));
        QCOMPARE(registry.integrationRecords("files").size(), 1);
        QVERIFY(!registry.integrationRecords("files").first().toMap().value("available").toBool());
        QVERIFY(!registry.setIntegrationEnabled("org.example.contributor", "files-action", true));
        QVERIFY(!registry.isEnabled("files"));
        QVERIFY(registry.setEnabled("files", true));
        auto record = registry.integrationRecords("files").first().toMap();
        QVERIFY(record.value("available").toBool());
        QVERIFY(!record.value("enabled").toBool());
        QVERIFY(registry.setIntegrationEnabled("org.example.contributor", "files-action", true));
        QVERIFY(registry.integrationRecords().first().toMap().value("enabled").toBool());
        QVERIFY(registry.setEnabled("files", false));
        QVERIFY(registry.isEnabled("org.example.contributor"));
        QVERIFY(!registry.integrationRecords().first().toMap().value("enabled").toBool());
        QVERIFY(registry.integrationRecords().first().toMap().value("configuredEnabled").toBool());
        QVERIFY(registry.setEnabled("files", true));
        QVERIFY(registry.integrationRecords().first().toMap().value("enabled").toBool());
        files["extensionPoints"] = QJsonArray{QJsonObject{{"id", "folder-actions"}, {"apiVersion", 2}}};
        package(root.filePath("installed/files/files.piext"), files);
        AddonRegistry next(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                           false);
        QVERIFY(next.isEnabled("files"));
        QVERIFY(next.isEnabled("org.example.contributor"));
        QVERIFY(!next.integrationRecords().first().toMap().value("available").toBool());
    }
    void integrationDefaultsApplyWithoutEnablingTargetsAndPreserveOptOut() {
        QTemporaryDir root;
        auto files = manifest("files");
        files["extensionPoints"] = QJsonArray{QJsonObject{{"id", "folder-actions"}, {"apiVersion", 1}}};
        auto contributor = manifest("org.example.contributor");
        contributor["integrations"] = QJsonArray{QJsonObject{
            {"id", "files"}, {"name", "Folder actions"}, {"target", "files"},
            {"minVersion", "1.0.0"}, {"point", "folder-actions"}, {"apiVersion", 1},
            {"entry", "Integration.qml"}, {"enabledByDefault", true}}};
        package(root.filePath("installed/files/files.piext"), files);
        package(root.filePath("installed/org.example.contributor/org.example.contributor.piext"), contributor);
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"), false);
        const auto record = [&] { return registry.integrationRecords("files").first().toMap(); };
        QVERIFY(record().value("configuredEnabled").toBool());
        QVERIFY(!record().value("enabled").toBool());
        QVERIFY(registry.setEnabled("org.example.contributor", true));
        QVERIFY(!registry.isEnabled("files"));
        QVERIFY(!record().value("enabled").toBool());
        QVERIFY(registry.setEnabled("files", true));
        QVERIFY(record().value("enabled").toBool());
        QVERIFY(!QSettings().contains("addons/integrations/org.example.contributor/files"));
        QVERIFY(registry.setEnabled("org.example.contributor", false));
        QVERIFY(!record().value("enabled").toBool());
        QVERIFY(registry.setEnabled("org.example.contributor", true));
        QVERIFY(record().value("enabled").toBool());
        QVERIFY(registry.setIntegrationEnabled("org.example.contributor", "files", false));
        QVERIFY(registry.setEnabled("org.example.contributor", false));
        QVERIFY(registry.setEnabled("org.example.contributor", true));
        QVERIFY(!record().value("configuredEnabled").toBool());
        QVERIFY(!record().value("enabled").toBool());
        AddonRegistry next(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"), false);
        QVERIFY(next.isEnabled("org.example.contributor"));
        QVERIFY(next.isEnabled("files"));
        QVERIFY(!next.integrationRecords("files").first().toMap().value("configuredEnabled").toBool());
        QVERIFY(!next.integrationRecords("files").first().toMap().value("enabled").toBool());
    }
    void pageAndSettingsCanBeAugmentedWithoutReservedExtensionPoints_data() {
        QTest::addColumn<QString>("point");
        QTest::addColumn<int>("apiVersion");
        QTest::addColumn<bool>("hasPage");
        QTest::addColumn<bool>("hasSettings");
        QTest::addColumn<bool>("available");
        QTest::newRow("page") << QString("page") << 1 << true << true << true;
        QTest::newRow("settings") << QString("settings") << 1 << true << true << true;
        QTest::newRow("unsupported-page-api") << QString("page") << 2 << true << true << false;
        QTest::newRow("unsupported-settings-api") << QString("settings") << 2 << true << true << false;
        QTest::newRow("unknown-custom-point") << QString("missing") << 1 << true << true << false;
        QTest::newRow("no-page") << QString("page") << 1 << false << true << false;
        QTest::newRow("no-settings") << QString("settings") << 1 << true << false << false;
    }
    void pageAndSettingsCanBeAugmentedWithoutReservedExtensionPoints() {
        QFETCH(QString, point);
        QFETCH(int, apiVersion);
        QFETCH(bool, hasPage);
        QFETCH(bool, hasSettings);
        QFETCH(bool, available);
        QTemporaryDir root;
        auto target = manifest("files");
        if (!hasPage) target.remove("page");
        if (!hasSettings) target.remove("settings");
        if (apiVersion != 1)
            target["extensionPoints"] = QJsonArray{QJsonObject{{"id", point}, {"apiVersion", apiVersion}}};
        auto contributor = manifest("org.example.contributor");
        contributor["integrations"] = QJsonArray{QJsonObject{
            {"id", "augment"}, {"name", "Augment Files"}, {"target", "files"},
            {"minVersion", "1.0.0"}, {"point", point}, {"apiVersion", apiVersion},
            {"entry", "Integration.qml"}}};
        package(root.filePath("installed/files/files.piext"), target);
        package(root.filePath("installed/org.example.contributor/org.example.contributor.piext"), contributor);
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"), false);
        QVERIFY(registry.setEnabled("files", true));
        QVERIFY(registry.setEnabled("org.example.contributor", true));
        const auto record = registry.integrationRecords("files").first().toMap();
        QCOMPARE(record.value("available").toBool(), available);
        QVERIFY(!record.value("configuredEnabled").toBool());
        QCOMPARE(registry.setIntegrationEnabled("org.example.contributor", "augment", true), available);
        QCOMPARE(registry.integrationRecords("files").first().toMap().value("enabled").toBool(), available);
    }
    void retiredBuiltinsDoNotPreventLoadingInstalledAddons() {
        QTemporaryDir root;
        const auto stateDirectory = root.filePath("state");
        QVERIFY(QDir().mkpath(stateDirectory));
        QFile state(stateDirectory + "/registry.json");
        QVERIFY(state.open(QIODevice::WriteOnly));
        const QJsonObject retired{{"id", "retired-addon"}, {"name", "Retired add-on"}, {"lifecycleDeclared", true}};
        const QJsonObject registryState{
            {"schema", 1}, {"seeded", QJsonArray{"retired-addon"}},
            {"owners", QJsonObject{{"retired-addon", retired}}},
            {"seedHashes", QJsonObject{{"retired-addon", QString(64, 'a')}}}};
        QVERIFY(state.write(QJsonDocument(registryState).toJson()) > 0);
        state.close();
        package(root.filePath("installed/retired-addon/retired-addon.piext"), manifest("retired-addon"));
        package(root.filePath("builtins/files.piext"), manifest("files"));

        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), stateDirectory, false);
        QVERIFY2(registry.error().isEmpty(), qPrintable(registry.error()));
        QVERIFY(registry.isEnabled("files"));
        QVERIFY(row(registry, "retired-addon").isEmpty());
        for (const auto &owner : registry.lifecycleOwners())
            QVERIFY(owner.toMap().value("id") != "retired-addon");
        QVERIFY(QFileInfo::exists(root.filePath("installed/retired-addon/retired-addon.piext")));
    }
    void shippedPackagesAreCopiedOnceAndDiscoveryIsAutomatic() {
        QTemporaryDir root;
        package(root.filePath("builtins/files.piext"), manifest("files"));
        package(root.filePath("builtins/devices.piext"), manifest("devices"));
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        QVERIFY(QFileInfo::exists(root.filePath("installed/files/files.piext")));
        QVERIFY(QFileInfo::exists(root.filePath("installed/devices/devices.piext")));
        QVERIFY(registry.isEnabled("files"));
        QVERIFY(!registry.isEnabled("devices"));
        package(root.filePath("installed/new.piext"), manifest());
        QTRY_VERIFY_WITH_TIMEOUT(!row(registry, "org.example.counter").isEmpty(), 3000);
        QVERIFY(!registry.isEnabled("org.example.counter"));
        QTest::qWait(300);
        QSignalSpy changes(&registry, &AddonRegistry::changed);
        QTest::qWait(500);
        QCOMPARE(changes.size(), 0);
        QVERIFY(QFile::remove(root.filePath("installed/org.example.counter/org.example.counter.piext")));
        QTRY_VERIFY_WITH_TIMEOUT(row(registry, "org.example.counter").value("restartRequired").toBool(),
                                 3000);
        QVERIFY(QFile::remove(root.filePath("installed/files/files.piext")));
        AddonRegistry restarted(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                                false);
        QVERIFY(!QFileInfo::exists(root.filePath("installed/files/files.piext")));
        QVERIFY(!restarted.isEnabled("files"));
        QVERIFY(!row(restarted, "files").value("error").toString().isEmpty());
    }
    void mountedPackageBytesSurviveInPlaceOverwriteAndDirectoryReplacement() {
        QTemporaryDir root;
        const auto path = root.filePath("installed/org.example.counter/org.example.counter.piext");
        package(path, manifest(), "immutable original bytes");
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        const auto url = row(registry, "org.example.counter").value("page").toUrl();
        QVERIFY(registry.setEnabled("org.example.counter", true));
        QFile changed(path);
        QVERIFY(changed.open(QIODevice::WriteOnly | QIODevice::Truncate));
        changed.write("incomplete replacement");
        changed.close();
        QTRY_VERIFY_WITH_TIMEOUT(row(registry, "org.example.counter").value("restartRequired").toBool(),
                                 3000);
        QFile retained(':' + url.path());
        QVERIFY(retained.open(QIODevice::ReadOnly));
        QCOMPARE(retained.readAll(), QByteArray("immutable original bytes"));
        QVERIFY(registry.isEnabled("org.example.counter"));
        QVERIFY(QDir(root.filePath("installed")).removeRecursively());
        QTRY_VERIFY_WITH_TIMEOUT(!registry.error().isEmpty(), 3000);
        QVERIFY(QDir().mkpath(root.filePath("installed")));
        package(root.filePath("installed/other.piext"), manifest("org.example.other"));
        QTRY_VERIFY_WITH_TIMEOUT(!row(registry, "org.example.other").isEmpty(), 3000);
    }
    void shippedUpgradesReplaceOnlyUnmodifiedSeededCopies() {
        QTemporaryDir root;
        auto shipped = manifest("files");
        const auto source = root.filePath("builtins/files.piext");
        package(source, shipped, "first shipped code");
        AddonRegistry first(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                            false);
        const auto oldPage = row(first, "files").value("page").toUrl();
        shipped["version"] = "1.1.0";
        package(source, shipped, "updated shipped code");
        AddonRegistry updated(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                              false);
        QCOMPARE(row(updated, "files").value("version").toString(), QString("1.1.0"));
        QFile original(':' + oldPage.path());
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("first shipped code"));
        package(source, shipped, "same-version rebuilt code");
        AddonRegistry rebuilt(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                              false);
        QFile current(':' + row(rebuilt, "files").value("page").toUrl().path());
        QVERIFY(current.open(QIODevice::ReadOnly));
        QCOMPARE(current.readAll(), QByteArray("same-version rebuilt code"));
        auto customized = shipped;
        customized["version"] = "9.0.0";
        package(root.filePath("installed/files/files.piext"), customized, "user customized code");
        shipped["version"] = "1.2.0";
        package(source, shipped, "newer shipped code");
        AddonRegistry custom(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                             false);
        QCOMPARE(row(custom, "files").value("version").toString(), QString("9.0.0"));
        QVERIFY(custom.setEnabled("files", false));
        QVERIFY(custom.finalizeRemoval("files"));
        AddonRegistry removed(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                              false);
        QVERIFY(!QFileInfo::exists(root.filePath("installed/files/files.piext")));
        QVERIFY(row(removed, "files").isEmpty());
    }
    void failedSeedUpgradeRestoresCodeAndCanRetry() {
        QTemporaryDir root;
        auto data = manifest("files");
        const auto source = root.filePath("builtins/files.piext");
        package(source, data, "old code");
        AddonRegistry first(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                            false);
        QFile installed(root.filePath("installed/files/files.piext"));
        QVERIFY(installed.open(QIODevice::ReadOnly));
        const auto original = installed.readAll();
        installed.close();
        data["version"] = "1.1.0";
        package(source, data, "new code");
        const auto folder = root.filePath("data/.registry");
        const auto permissions = QFileInfo(folder).permissions();
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, permissions); });
        {
            AddonRegistry failed(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                                 false);
            QVERIFY(!failed.error().isEmpty());
        }
        QVERIFY(QFile::setPermissions(folder, permissions));
        QVERIFY(installed.open(QIODevice::ReadOnly));
        QCOMPARE(installed.readAll(), original);
        installed.close();
        AddonRegistry retry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                            false);
        QCOMPARE(row(retry, "files").value("version").toString(), QString("1.1.0"));
    }
    void removalRequiresCleanupAndKeepsDataAndOwnershipReceipts() {
        QTemporaryDir root;
        auto data = manifest();
        data["lifecycle"] = "Lifecycle.qml";
        package(root.filePath("installed/input.piext"), data);
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        auto owner = registry.lifecycleOwners().first().toMap();
        QVERIFY(owner.value("lifecycleDeclared").toBool());
        QVERIFY(!owner.value("missing").toBool());
        QVERIFY(!owner.value("lifecycle").toUrl().isEmpty());
        const auto directory = registry.dataDirectory("org.example.counter");
        QFile saved(directory + "/saved.txt");
        QVERIFY(saved.open(QIODevice::WriteOnly));
        saved.write("keep saved work");
        saved.close();
        QVERIFY(registry.setEnabled("org.example.counter", true));
        QVERIFY(!registry.finalizeRemoval("org.example.counter", true));
        QVERIFY(registry.setEnabled("org.example.counter", false));
        QVERIFY(!registry.finalizeRemoval("org.example.counter"));
        QVERIFY(!registry.finalizeRemoval("org.example.counter", true,
                                          {{"personal", QStringList{"../outside"}}}));
        QVERIFY(!registry.finalizeRemoval("org.example.counter", true, {{"unknown", QStringList{"Notes"}}}));
        QVERIFY(QFileInfo::exists(root.filePath("installed/org.example.counter/org.example.counter.piext")));
        const QVariantMap paths{{"personal", QStringList{"Notes"}},
                                {"downloads", QStringList{"models"}},
                                {"addonData", QStringList{"addon-data/org.example.counter"}}};
        QVERIFY(registry.finalizeRemoval("org.example.counter", true, paths));
        QVERIFY(row(registry, "org.example.counter").isEmpty());
        QVERIFY(!QFileInfo::exists(root.filePath("installed/org.example.counter/org.example.counter.piext")));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QByteArray("keep saved work"));
        AddonRegistry next(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                           false);
        owner = next.lifecycleOwners().first().toMap();
        QVERIFY(owner.value("removed").toBool());
        QVERIFY(!owner.value("missing").toBool());
        QVERIFY(!owner.value("lifecycleDeclared").toBool());
        QCOMPARE(owner.value("ownedPaths").toMap().value("personal").toList(),
                 QVariantList{QString("Notes")});
    }
    void missingLifecycleOwnersAndFailedRemovalRemainRetryable() {
        QTemporaryDir root;
        auto data = manifest("files");
        data["lifecycle"] = "Lifecycle.qml";
        package(root.filePath("builtins/files.piext"), data);
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        QVERIFY(registry.setEnabled("files", false));
        const auto state = root.filePath("data/.registry");
        const auto permissions = QFileInfo(state).permissions();
        QVERIFY(QFile::setPermissions(state, QFile::ReadOwner | QFile::ExeOwner));
        QVERIFY(!registry.finalizeRemoval("files", true));
        QVERIFY(QFileInfo::exists(root.filePath("installed/files/files.piext")));
        QVERIFY(!registry.lifecycleOwners().first().toMap().value("removed").toBool());
        QVERIFY(QFile::setPermissions(state, permissions));
        QVERIFY(QFile::remove(root.filePath("installed/files/files.piext")));
        AddonRegistry missing(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                              false);
        const auto owner = missing.lifecycleOwners().first().toMap();
        QVERIFY(owner.value("missing").toBool());
        QVERIFY(owner.value("lifecycleDeclared").toBool());
        QVERIFY(owner.value("lifecycle").toUrl().isEmpty());
        QVERIFY(!missing.finalizeRemoval("files"));
        QVERIFY(!QFileInfo::exists(root.filePath("installed/files/files.piext")));
        QVERIFY(missing.finalizeRemoval("files", true));
        AddonRegistry removed(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                              false);
        QVERIFY(row(removed, "files").isEmpty());
    }
    void replacementCannotForgetAnEarlierCleanupRequirement() {
        QTemporaryDir root;
        auto data = manifest();
        data["lifecycle"] = "Lifecycle.qml";
        const auto path = root.filePath("installed/org.example.counter/org.example.counter.piext");
        package(path, data);
        AddonRegistry first(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                            false);
        QVERIFY(first.lifecycleOwners().first().toMap().value("lifecycleDeclared").toBool());
        data.remove("lifecycle");
        data["version"] = "2.0.0";
        package(path, data);
        AddonRegistry next(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                           false);
        const auto owner = next.lifecycleOwners().first().toMap();
        QVERIFY(owner.value("lifecycleDeclared").toBool());
        QVERIFY(owner.value("lifecycle").toUrl().isEmpty());
        QVERIFY(!next.finalizeRemoval("org.example.counter"));
        QVERIFY(QFileInfo::exists(path));
    }
    void duplicateIdsAndMalformedFilesDoNotHideValidPackages() {
        QTemporaryDir root;
        package(root.filePath("installed/one.piext"), manifest());
        package(root.filePath("installed/two.piext"), manifest());
        package(root.filePath("installed/good.piext"), manifest("org.example.other"));
        QFile broken(root.filePath("installed/broken.piext"));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("not an RCC file");
        broken.close();
        QVERIFY(QFile::link(root.filePath("installed/good.piext"), root.filePath("installed/linked.piext")));
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        int duplicates = 0, errors = 0;
        for (const auto &entry : registry.addons()) {
            const auto item = entry.toMap();
            if (item.value("id") == "org.example.counter") {
                ++duplicates;
                QVERIFY(item.value("error").toString().contains("Duplicate"));
            }
            if (!item.value("error").toString().isEmpty())
                ++errors;
        }
        QCOMPARE(duplicates, 2);
        QVERIFY(errors >= 4);
        QVERIFY(!registry.setEnabled("org.example.counter", true));
        QVERIFY(registry.setEnabled("org.example.other", true));
        QVERIFY(registry.isEnabled("org.example.other"));
    }
    void recoverySuppressesActivationWithoutLosingSavedChoices() {
        QTemporaryDir root;
        package(root.filePath("builtins/files.piext"), manifest("files"));
        package(root.filePath("installed/counter.piext"), manifest());
        QSettings().setValue("addons/enabled/org.example.counter", true);
        AddonRegistry recovery(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               true);
        QVERIFY(recovery.recovery());
        QVERIFY(!recovery.isEnabled("files"));
        QVERIFY(!recovery.isEnabled("org.example.counter"));
        QVERIFY(row(recovery, "org.example.counter").value("configuredEnabled").toBool());
        QVERIFY(recovery.setEnabled("org.example.counter", false));
        QVERIFY(!recovery.isEnabled("org.example.counter"));
        AddonRegistry normal(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                             false);
        QVERIFY(normal.isEnabled("files"));
        QVERIFY(!normal.isEnabled("org.example.counter"));
    }
    void refreshRejectsNewCollisionsWithoutInvalidatingTheRegisteredOwner() {
        QTemporaryDir root;
        package(root.filePath("installed/owner.piext"), manifest(), "owner content");
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        QVERIFY(registry.setEnabled("org.example.counter", true));
        const auto ownerUrl = row(registry, "org.example.counter").value("page").toUrl();
        auto newcomer = manifest();
        newcomer["name"] = "Conflicting newcomer";
        package(root.filePath("installed/aaa-newcomer.piext"), newcomer, "new content");
        registry.refresh();
        int accepted = 0, rejected = 0;
        for (const auto &value : registry.addons()) {
            const auto item = value.toMap();
            if (item.value("id") != "org.example.counter")
                continue;
            if (item.value("error").toString().isEmpty()) {
                ++accepted;
                QCOMPARE(item.value("page").toUrl(), ownerUrl);
                QVERIFY(item.value("enabled").toBool());
            } else
                ++rejected;
        }
        QCOMPARE(accepted, 1);
        QCOMPARE(rejected, 1);
        QVERIFY(registry.isEnabled("org.example.counter"));
        registry.refresh();
        QVERIFY(registry.isEnabled("org.example.counter"));
        AddonRegistry firstScan(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                                false);
        QVERIFY(!firstScan.isEnabled("org.example.counter"));
        firstScan.refresh();
        QVERIFY(!firstScan.isEnabled("org.example.counter"));
        QVERIFY(QFile::remove(root.filePath("installed/aaa-newcomer.piext")));
        firstScan.refresh();
        QVERIFY(firstScan.isEnabled("org.example.counter"));
    }
    void failedChoiceWriteKeepsThePreviousEnabledState() {
        QTemporaryDir root;
        package(root.filePath("installed/counter.piext"), manifest());
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        QVERIFY(registry.setEnabled("org.example.counter", true));
        const auto folder = QFileInfo(QSettings().fileName()).absolutePath();
        const auto permissions = QFileInfo(folder).permissions();
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, permissions); });
        QVERIFY(!registry.setEnabled("org.example.counter", false));
        QVERIFY(registry.isEnabled("org.example.counter"));
        QVERIFY(!registry.error().isEmpty());
    }
    void scopedPreferencesArePrivatePersistedAndProtectedFromLinks() {
        QTemporaryDir root;
        package(root.filePath("installed/one.piext"), manifest());
        package(root.filePath("installed/two.piext"), manifest("org.example.other"));
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        QVERIFY(registry.setSetting("org.example.counter", "value", 12));
        QVERIFY(registry.setSetting("org.example.other", "value", 34));
        QCOMPARE(registry.setting("org.example.counter", "value").toInt(), 12);
        QCOMPARE(registry.setting("org.example.other", "value").toInt(), 34);
        const auto data = registry.dataDirectory("org.example.counter");
        const auto otherBits = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther |
                               QFile::WriteOther | QFile::ExeOther;
        QVERIFY(!(QFileInfo(data).permissions() & otherBits));
        QVERIFY(!(QFileInfo(data + "/settings.ini").permissions() & otherBits));
        QVERIFY(registry.setEnabled("org.example.counter", false));
        QCOMPARE(registry.setting("org.example.counter", "value").toInt(), 12);
        QVERIFY(!registry.setSetting("../escape", "value", 5));
        QVERIFY(!registry.setSetting("org.example.counter", "../escape", 5));
        QFile victim(root.filePath("victim"));
        QVERIFY(victim.open(QIODevice::WriteOnly));
        victim.write("preserve");
        victim.close();
        QVERIFY(QFile::remove(data + "/settings.ini"));
        QVERIFY(QFile::link(victim.fileName(), data + "/settings.ini"));
        QVERIFY(!registry.setSetting("org.example.counter", "value", 99));
        QVERIFY(victim.open(QIODevice::ReadOnly));
        QCOMPARE(victim.readAll(), QByteArray("preserve"));
    }
    void settingsPreflightChecksPermissionsAndLinksWithoutChangingData() {
        QTemporaryDir root;
        package(root.filePath("installed/input.piext"), manifest());
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        const auto directory = registry.dataDirectory("org.example.counter");
        const auto path = directory + "/settings.ini";
        QVERIFY(registry.settingsWritable("org.example.counter"));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(registry.setSetting("org.example.counter", "step", 7));
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto bytes = saved.readAll();
        saved.close();
        const auto entries =
            QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        QVERIFY(registry.settingsWritable("org.example.counter"));
        QCOMPARE(QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), entries);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), bytes);
        saved.close();
        const auto filePermissions = QFileInfo(path).permissions();
        QVERIFY(QFile::setPermissions(path, QFile::ReadOwner));
        auto restoreFile = qScopeGuard([&] { QFile::setPermissions(path, filePermissions); });
        QVERIFY(!registry.settingsWritable("org.example.counter"));
        QVERIFY(!registry.error().isEmpty());
        QVERIFY(QFile::setPermissions(path, filePermissions));
        restoreFile.dismiss();
        const auto directoryPermissions = QFileInfo(directory).permissions();
        QVERIFY(QFile::setPermissions(directory, QFile::ReadOwner | QFile::ExeOwner));
        const auto restoreDirectory =
            qScopeGuard([&] { QFile::setPermissions(directory, directoryPermissions); });
        QVERIFY(!registry.settingsWritable("org.example.counter"));
        QVERIFY(!registry.error().isEmpty());
        QVERIFY(QFile::setPermissions(directory, directoryPermissions));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), bytes);
        saved.close();
        QFile victim(root.filePath("victim.ini"));
        QVERIFY(victim.open(QIODevice::WriteOnly));
        victim.write("preserve target");
        victim.close();
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::link(victim.fileName(), path));
        QVERIFY(!registry.settingsWritable("org.example.counter"));
        QVERIFY(!registry.error().isEmpty());
        QVERIFY(victim.open(QIODevice::ReadOnly));
        QCOMPARE(victim.readAll(), QByteArray("preserve target"));
        QVERIFY(!registry.settingsWritable("org.example.unknown"));
        QVERIFY(!registry.settingsWritable("../escape"));
        auto invalid = manifest("org.example.invalid");
        invalid["lifecycle"] = "../Lifecycle.qml";
        package(root.filePath("installed/invalid.piext"), invalid);
        registry.refresh();
        QVERIFY(!registry.settingsWritable("org.example.invalid"));
    }
    void installIsValidatedAtomicAndUpdatesWaitForRestart() {
        QTemporaryDir root;
        const auto source = root.filePath("source.piext");
        package(source, manifest(), "first version");
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        QVERIFY(registry.install(QUrl::fromLocalFile(source)));
        QVERIFY(!registry.isEnabled("org.example.counter"));
        const auto first = row(registry, "org.example.counter");
        QVERIFY(first.value("error").toString().isEmpty());
        const auto installed = root.filePath("installed/org.example.counter/org.example.counter.piext");
        QVERIFY(QFileInfo::exists(installed));
        QFile before(installed);
        QVERIFY(before.open(QIODevice::ReadOnly));
        const auto bytes = before.readAll();
        before.close();
        QFile bad(root.filePath("bad.piext"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("not a package");
        bad.close();
        QVERIFY(!registry.install(QUrl::fromLocalFile(bad.fileName())));
        QVERIFY(before.open(QIODevice::ReadOnly));
        QCOMPARE(before.readAll(), bytes);
        before.close();
        auto updated = manifest();
        updated["version"] = "2.0.0";
        package(source, updated, "second version");
        QVERIFY(registry.install(QUrl::fromLocalFile(source)));
        QCOMPARE(row(registry, "org.example.counter").value("version").toString(), QString("1.0.0"));
        QVERIFY(row(registry, "org.example.counter").value("restartRequired").toBool());
        QVERIFY(registry.error().contains("Restart"));
        QFile oldResource(':' + first.value("page").toUrl().path());
        QVERIFY(oldResource.open(QIODevice::ReadOnly));
        QCOMPARE(oldResource.readAll(), QByteArray("first version"));
        AddonRegistry restarted(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                                false);
        QCOMPARE(row(restarted, "org.example.counter").value("version").toString(), QString("2.0.0"));
    }
    void compileToolBuildsTheExampleAndRejectsSourceLinks() {
        QTemporaryDir root;
        const auto project =
            QDir(QStringLiteral(PILON_SOURCE_ROOT)).absolutePath();
        const auto python = QStandardPaths::findExecutable("python3");
        QVERIFY(!python.isEmpty());
        const auto script = project + "/src/scripts/compile-addon.py";
        QProcess compile;
        compile.start(python, {script, project + "/examples/counter", "--output",
                               root.filePath("example.piext"), "--rcc", rcc});
        QVERIFY(compile.waitForFinished(10000));
        QVERIFY2(compile.exitCode() == 0, compile.readAllStandardError().constData());
        AddonRegistry registry(root.filePath("builtins"), root.filePath("installed"), root.filePath("data/.registry"),
                               false);
        QVERIFY(registry.install(QUrl::fromLocalFile(root.filePath("example.piext"))));
        QVERIFY(QDir().mkpath(root.filePath("unsafe")));
        QFile manifestFile(root.filePath("unsafe/manifest.json"));
        QVERIFY(manifestFile.open(QIODevice::WriteOnly));
        manifestFile.write(QJsonDocument(manifest()).toJson());
        manifestFile.close();
        QVERIFY(QFile::link(project + "/examples/counter/Page.qml", root.filePath("unsafe/Page.qml")));
        compile.start(python, {script, root.filePath("unsafe"), "--output", root.filePath("rejected.piext"),
                               "--rcc", rcc});
        QVERIFY(compile.waitForFinished(10000));
        QVERIFY(compile.exitCode() != 0);
        QVERIFY(!QFileInfo::exists(root.filePath("rejected.piext")));
    }
};
int main(int argc, char **argv) {
    QTemporaryDir settings;
    qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("PilonTests");
    QCoreApplication::setApplicationName("Addons");
    AddonTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "addon_tests.moc"
