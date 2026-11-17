#include "tailscalemanager.h"
#include "pilonsettings.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QScopeGuard>
#include <QBuffer>
#include <QUrlQuery>
#include <QTimer>
#include <QFutureWatcher>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

class DiscoveryReply : public QNetworkReply {
    QByteArray payload;
    qint64 offset = 0;
public:
    DiscoveryReply(const QNetworkRequest &request, int status, QByteArray data, int delay, QObject *parent)
        : QNetworkReply(parent), payload(std::move(data)) {
        setRequest(request);setUrl(request.url());setAttribute(QNetworkRequest::HttpStatusCodeAttribute,status);
        open(QIODevice::ReadOnly);
        if (delay >= 0) QTimer::singleShot(delay,this,[this] { emit readyRead();setFinished(true);emit finished(); });
    }
    void abort() override { setError(OperationCanceledError,"Cancelled");setFinished(true);emit finished(); }
    qint64 bytesAvailable() const override { return payload.size()-offset+QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *data,qint64 maximum) override {
        const auto count=qMin(maximum,payload.size()-offset);
        if (count<=0) return -1;
        memcpy(data,payload.constData()+offset,size_t(count));offset+=count;return count;
    }
};
class DiscoveryNetwork : public QNetworkAccessManager {
public:
    struct Response {int status=200;QByteArray body;int delay=0;};
    QList<Response> responses;
    QList<QNetworkRequest> requests;
    QList<QByteArray> bodies;
protected:
    QNetworkReply *createRequest(Operation operation,const QNetworkRequest &request,QIODevice *data) override {
        Q_UNUSED(operation);
        requests.append(request);bodies.append(data ? data->readAll() : QByteArray());
        const auto response=responses.isEmpty() ? Response{500,{}} : responses.takeFirst();
        return new DiscoveryReply(request,response.status,response.body,response.delay,this);
    }
};

class DeviceTests : public QObject {
    Q_OBJECT
    QTemporaryDir fixture;
    QByteArray originalPath;
    bool write(const QString &name,const QByteArray &bytes) {
        QFile file(fixture.filePath(name));
        return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size();
    }
    bool status(const QString &identity="fixture-account",const QString &state="Running") {
        return write("status",QJsonDocument(QJsonObject{{"BackendState",state},{"Self",QJsonObject{{"ID",identity},{"UserID",7},{"HostName","Fixture"}}}}).toJson());
    }
    QByteArray calls() {QFile file(fixture.filePath("calls"));return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};}
    QByteArray credential() {QFile file(fixture.filePath("credential"));return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};}
    DiscoveryNetwork *network(TailscaleManager &manager) {
        auto mock=std::make_unique<DiscoveryNetwork>();auto *result=mock.get();manager.m_discoveryNetwork=std::move(mock);return result;
    }
    QByteArray token(const QByteArray &value="fixture-access") {
        return QJsonDocument(QJsonObject{{"access_token",QString::fromLatin1(value)},{"token_type","Bearer"},{"expires_in",3600},{"scope","devices:core:read"}}).toJson();
    }
    QByteArray peers(const QString &id="peer-a",const QString &ip="100.64.0.2") {
        return QJsonDocument(QJsonObject{{"devices",QJsonArray{QJsonObject{{"nodeId",id},{"hostname","Fixture peer"},{"addresses",QJsonArray{ip}},{"connectedToControl",true}}}}}).toJson();
    }
private slots:
    void initTestCase() {
        QVERIFY(fixture.isValid());
        QCoreApplication::setOrganizationName("PilonTests");QCoreApplication::setApplicationName("Devices");
        QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,fixture.path());
        QVERIFY(write("tailscale",R"(#!/bin/sh
printf '%s\n' "$*" >> "$PILON_DEVICE_TEST_CALLS"
case "$1" in
status) cat "$PILON_DEVICE_TEST_STATUS" ;;
logout) printf '%s\n' '{"BackendState":"NeedsLogin"}' > "$PILON_DEVICE_TEST_STATUS" ;;
login) exit 0 ;;
*) exit 1 ;;
esac
)"));
        QVERIFY(write("pkexec","#!/bin/sh\nexec \"$@\"\n"));
        QVERIFY(write("secret-tool",R"(#!/bin/sh
if [ "$PILON_DEVICE_KEYRING_FAIL" = "$1" ]; then printf 'Fixture failure' >&2; exit 2; fi
if [ "$PILON_DEVICE_KEYRING_DELAY" = "$1" ]; then
 printf ready > "$PILON_DEVICE_KEYRING_STARTED"
 sleep 0.3
fi
case "$1" in
lookup) if [ -f "$PILON_DEVICE_TEST_CREDENTIAL" ]; then cat "$PILON_DEVICE_TEST_CREDENTIAL"; else exit 1; fi ;;
store) cat > "$PILON_DEVICE_TEST_CREDENTIAL" ;;
clear) rm -f "$PILON_DEVICE_TEST_CREDENTIAL" ;;
*) exit 2 ;;
esac
)"));
        for(const auto &tool:{"tailscale","pkexec","secret-tool"})QVERIFY(QFile::setPermissions(fixture.filePath(tool),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        originalPath=qgetenv("PATH");qputenv("PATH",fixture.path().toUtf8()+':'+originalPath);
        qputenv("PILON_DEVICE_TEST_CALLS",fixture.filePath("calls").toUtf8());
        qputenv("PILON_DEVICE_TEST_STATUS",fixture.filePath("status").toUtf8());
        qputenv("PILON_DEVICE_TEST_CREDENTIAL",fixture.filePath("credential").toUtf8());
        qputenv("PILON_DEVICE_KEYRING_STARTED",fixture.filePath("keyring-started").toUtf8());
    }
    void cleanupTestCase(){qputenv("PATH",originalPath);qunsetenv("PILON_DEVICE_TEST_CALLS");qunsetenv("PILON_DEVICE_TEST_STATUS");}
    void init(){PilonSettings().clear();QVERIFY(write("calls",{}));QVERIFY(status());QFile::remove(fixture.filePath("credential"));QFile::remove(fixture.filePath("keyring-started"));qunsetenv("PILON_DEVICE_KEYRING_FAIL");qunsetenv("PILON_DEVICE_KEYRING_DELAY");}
    void discoveryUsesScopedOfficialRequestsAndRestoresFromOnlyTheSecureStore() {
        TailscaleManager manager;auto *http=network(manager);
        http->responses={{200,token()},{200,peers()}};
        manager.connectDiscovery("fixture-client","fixture-client-secret-a");QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.discoveryConnected());QVERIFY(manager.errorMessage().isEmpty());
        QCOMPARE(http->requests.size(),2);
        QCOMPARE(http->requests[0].url(),QUrl("https://api.tailscale.com/api/v2/oauth/token"));
        QCOMPARE(http->requests[1].url(),QUrl("https://api.tailscale.com/api/v2/tailnet/-/devices"));
        const QUrlQuery form(QString::fromUtf8(http->bodies[0]));
        QCOMPARE(form.queryItemValue("grant_type"),QString("client_credentials"));
        QCOMPARE(form.queryItemValue("scope"),QString("devices:core:read"));
        QCOMPARE(form.queryItemValue("client_secret"),QString("fixture-client-secret-a"));
        QCOMPARE(http->requests[1].rawHeader("Authorization"),QByteArray("Bearer fixture-access"));
        for(const auto &request:http->requests) QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),int(QNetworkRequest::ManualRedirectPolicy));
        QVERIFY(http->bodies[1].isEmpty());QVERIFY(!PilonSettings().allKeys().contains("tailscale/discoveryEnabled"));
        for(const auto &key:PilonSettings().allKeys()) QVERIFY(!PilonSettings().value(key).toString().contains("fixture-client-secret"));
        QCOMPARE(manager.devices().size(),1);const auto peer=manager.devices().first().toMap();
        QCOMPARE(peer.value("id").toString(),QString("api:peer-a"));QVERIFY(!peer.value("onlineKnown").toBool());
        manager.setDeviceIncluded("api:peer-a",true);QCOMPARE(manager.includedDevices().size(),1);
        QVERIFY(!manager.loggedIn());QVERIFY(!manager.usedByPilon());
        TailscaleManager restarted;restarted.restoreDiscovery();QTRY_VERIFY(!restarted.busy());
        QVERIFY(restarted.discoveryConnected());QCOMPARE(restarted.m_discoveryCredential,credential());
        restarted.disconnectDiscovery();QTRY_VERIFY(!restarted.busy());
        QVERIFY(!restarted.discoveryConnected());QVERIFY(credential().isEmpty());QVERIFY(restarted.devices().isEmpty());
    }
    void failedReplacementCannotReuseTheNewAccountsToken() {
        TailscaleManager manager;auto *http=network(manager);
        http->responses={{200,token("token-a")},{200,peers()}};
        manager.connectDiscovery("client-a","fixture-client-secret-a");QTRY_VERIFY(!manager.busy());
        const auto previous=credential();
        http->responses={{200,token("token-b")},{403,"{\"error\":\"do not expose this body\"}"}};
        manager.connectDiscovery("client-b","fixture-client-secret-b");QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.discoveryConnected());QCOMPARE(credential(),previous);QVERIFY(manager.m_discoveryToken.isEmpty());
        QVERIFY(!manager.errorMessage().contains("do not expose"));
        http->responses={{200,token("token-a-refreshed")},{200,peers()}};
        manager.refreshDiscovery();QTRY_VERIFY(!manager.busy());
        const QUrlQuery refreshed(QString::fromUtf8(http->bodies[4]));
        QCOMPARE(refreshed.queryItemValue("client_id"),QString("client-a"));
        QCOMPARE(http->requests.last().rawHeader("Authorization"),QByteArray("Bearer token-a-refreshed"));
    }
    void invalidDiscoveryAndFailedStorageNeverPublishOrPersistCredentials() {
        TailscaleManager manager;auto *http=network(manager);
        manager.connectDiscovery("client\nheader","fixture-client-secret");QVERIFY(http->requests.isEmpty());
        http->responses={{200,token()},{200,"{\"devices\":[{\"nodeId\":\"bad\"}]}"}};
        manager.connectDiscovery("client","fixture-client-secret");QTRY_VERIFY(!manager.busy());
        QVERIFY(!manager.discoveryConnected());QVERIFY(manager.devices().isEmpty());QVERIFY(credential().isEmpty());
        qputenv("PILON_DEVICE_KEYRING_FAIL","store");
        http->responses={{200,token()},{200,peers()}};
        manager.connectDiscovery("client","fixture-client-secret");QTRY_VERIFY(!manager.busy());
        QVERIFY(!manager.discoveryConnected());QVERIFY(manager.devices().isEmpty());QVERIFY(credential().isEmpty());
        QVERIFY(manager.errorMessage().contains("credential store"));
    }
    void discoveryTimeoutDiscardsLateRepliesAndKeepsManualHosts() {
        TailscaleManager manager;auto *http=network(manager);
        QVERIFY(manager.saveDevice({},"Manual","100.64.0.99"));
        http->responses={{200,token(),100}};
        manager.connectDiscovery("client","fixture-client-secret");QTRY_COMPARE(http->requests.size(),1);
        QVERIFY(QMetaObject::invokeMethod(&manager.m_timeout,"timeout"));QVERIFY(!manager.busy());
        http->responses={{200,token("replacement")},{200,peers("replacement")}};
        manager.connectDiscovery("replacement-client","fixture-client-secret-replacement");QTRY_VERIFY(!manager.busy());
        QTest::qWait(150);QVERIFY(manager.discoveryConnected());
        QCOMPARE(manager.savedDevices().size(),1);QCOMPARE(http->requests.size(),3);
        QCOMPARE(QJsonDocument::fromJson(credential()).object().value("clientId").toString(),QString("replacement-client"));
        const auto devices=manager.devices();
        QVERIFY(std::any_of(devices.cbegin(),devices.cend(),[](const auto &entry) {return entry.toMap().value("id").toString()=="api:replacement";}));
    }
    void cancelledStoreAndClearRollBackBeforeTheNextKeyringRead() {
        TailscaleManager manager;auto *http=network(manager);
        http->responses={{200,token()},{200,peers()}};
        qputenv("PILON_DEVICE_KEYRING_DELAY","store");
        manager.connectDiscovery("client","fixture-client-secret");
        QTRY_VERIFY(QFile::exists(fixture.filePath("keyring-started")));
        QVERIFY(QMetaObject::invokeMethod(&manager.m_timeout,"timeout"));
        TailscaleManager restarted;restarted.restoreDiscovery();QTRY_VERIFY(!restarted.busy());
        QVERIFY(!restarted.discoveryConnected());QVERIFY(credential().isEmpty());
        qunsetenv("PILON_DEVICE_KEYRING_DELAY");
        http->responses={{200,token()},{200,peers()}};
        manager.connectDiscovery("client","fixture-client-secret");QTRY_VERIFY(!manager.busy());
        const auto previous=credential();QFile::remove(fixture.filePath("keyring-started"));
        qputenv("PILON_DEVICE_KEYRING_DELAY","clear");
        manager.disconnectDiscovery();QTRY_VERIFY(QFile::exists(fixture.filePath("keyring-started")));
        QVERIFY(QMetaObject::invokeMethod(&manager.m_timeout,"timeout"));
        restarted.restoreDiscovery();QTRY_VERIFY(!restarted.busy());
        QVERIFY(restarted.discoveryConnected());QCOMPARE(credential(),previous);
    }
    void failedCancellationRollbackRequiresExplicitRecovery() {
        TailscaleManager manager;auto *http=network(manager);
        http->responses={{200,token()},{200,peers()}};
        manager.connectDiscovery("client","fixture-client-secret");QTRY_VERIFY(!manager.busy());
        qputenv("PILON_DEVICE_KEYRING_DELAY","clear");qputenv("PILON_DEVICE_KEYRING_FAIL","store");
        manager.disconnectDiscovery();QTRY_VERIFY(QFile::exists(fixture.filePath("keyring-started")));
        QVERIFY(QMetaObject::invokeMethod(&manager.m_timeout,"timeout"));
        QTRY_VERIFY(manager.errorMessage().contains("could not restore"));
        QVERIFY(!manager.discoveryConnected());QVERIFY(credential().isEmpty());
    }
    void staleRecoveryFailureCannotReplaceANewerAcceptedConnection() {
        TailscaleManager manager;auto *http=network(manager);
        http->responses={{200,token()},{200,peers()}};
        manager.connectDiscovery("client-a","fixture-client-secret-a");QTRY_VERIFY(!manager.busy());
        qputenv("PILON_DEVICE_KEYRING_DELAY","clear");qputenv("PILON_DEVICE_KEYRING_FAIL","store");
        manager.disconnectDiscovery();QTRY_VERIFY(QFile::exists(fixture.filePath("keyring-started")));
        QFutureWatcherBase *pending=nullptr;
        for(auto *watcher:manager.findChildren<QFutureWatcherBase *>()) if(watcher->isRunning()) pending=watcher;
        QVERIFY(pending);QSignalBlocker blocked(pending);
        QVERIFY(QMetaObject::invokeMethod(&manager.m_timeout,"timeout"));
        QTRY_VERIFY(pending->isFinished());
        qunsetenv("PILON_DEVICE_KEYRING_DELAY");qunsetenv("PILON_DEVICE_KEYRING_FAIL");
        http->responses={{200,token("token-c")},{200,peers("peer-c")}};
        manager.connectDiscovery("client-c","fixture-client-secret-c");QTRY_VERIFY(!manager.busy());
        const auto committed=credential();
        blocked.unblock();QVERIFY(QMetaObject::invokeMethod(pending,"finished"));
        QVERIFY(manager.discoveryConnected());QVERIFY(manager.errorMessage().isEmpty());
        QCOMPARE(manager.m_discoveryToken,QByteArray("token-c"));QCOMPARE(credential(),committed);
        QCOMPARE(manager.m_discoveryDevices.first().toMap().value("id").toString(),QString("api:peer-c"));
    }
    void removedSecureRecordDisconnectsDiscoveryAndPreservesManualHosts() {
        TailscaleManager manager;auto *http=network(manager);
        QVERIFY(manager.saveDevice({},"Manual","100.64.0.99"));
        http->responses={{200,token()},{200,peers()}};
        manager.connectDiscovery("client","fixture-client-secret");QTRY_VERIFY(!manager.busy());
        QVERIFY(QFile::remove(fixture.filePath("credential")));
        manager.refreshDiscovery();QTRY_VERIFY(!manager.busy());
        QVERIFY(!manager.discoveryConnected());QVERIFY(manager.m_discoveryDevices.isEmpty());
        QCOMPARE(manager.devices().size(),1);QVERIFY(manager.devices().first().toMap().value("saved").toBool());
    }
    void deviceTimeoutDuringReplacementClearsTheCandidateToken() {
        TailscaleManager manager;auto *http=network(manager);
        http->responses={{200,token("token-a")},{200,peers()}};
        manager.connectDiscovery("client-a","fixture-client-secret-a");QTRY_VERIFY(!manager.busy());
        http->responses={{200,token("token-b")},{200,peers("peer-b"),-1}};
        manager.connectDiscovery("client-b","fixture-client-secret-b");QTRY_COMPARE(http->requests.size(),4);
        QVERIFY(QMetaObject::invokeMethod(&manager.m_timeout,"timeout"));
        QVERIFY(manager.m_discoveryToken.isEmpty());QVERIFY(manager.discoveryConnected());
        http->responses={{200,token("token-a-restored")},{200,peers()}};
        manager.refreshDiscovery();QTRY_VERIFY(!manager.busy());
        QCOMPARE(QUrlQuery(QString::fromUtf8(http->bodies[4])).queryItemValue("client_id"),QString("client-a"));
    }
    void discoveryRejectsRedirectsBroadScopesAndOversizedReplies() {
        for(const auto &response:QList<DiscoveryNetwork::Response>{
            {302,"redirect"},{200,"{\"access_token\":\"fixture\",\"token_type\":\"Bearer\",\"scope\":\"all\"}"},
            {200,QByteArray(4*1024*1024+1,'x')}}) {
            TailscaleManager manager;auto *http=network(manager);http->responses={response};
            manager.connectDiscovery("client","fixture-client-secret");QTRY_VERIFY(!manager.busy());
            QVERIFY(!manager.discoveryConnected());QVERIFY(credential().isEmpty());QCOMPARE(http->requests.size(),1);
            QVERIFY(!manager.errorMessage().isEmpty());
        }
    }
    void committedCredentialWaitsForItsCompletionInsteadOfTimingOut() {
        TailscaleManager manager;
        manager.setBusy(true);manager.m_discoveryCommitState=std::make_shared<std::atomic<TailscaleManager::DiscoveryCommit>>(TailscaleManager::DiscoveryCommit::Committed);
        QVERIFY(QMetaObject::invokeMethod(&manager.m_timeout,"timeout"));
        QVERIFY(manager.busy());QVERIFY(manager.errorMessage().isEmpty());
        manager.finishDiscovery();
    }
    void destructionCancelsPendingDiscoveryWithoutSavingCredentials() {
        auto manager=std::make_unique<TailscaleManager>();auto *http=network(*manager);
        http->responses={{200,token(),-1}};
        manager->connectDiscovery("client","fixture-client-secret");QTRY_COMPARE(http->requests.size(),1);
        manager.reset();QVERIFY(credential().isEmpty());
    }
    void desktopInstallationRefreshDetectsAClientInstalledAfterStartup() {
        const auto path=qgetenv("PATH");
        const auto restore=qScopeGuard([&] { qputenv("PATH",path); });
        QVERIFY(QDir().mkpath(fixture.filePath("empty-path")));
        qputenv("PATH",fixture.filePath("empty-path").toUtf8());
        TailscaleManager manager;
        QVERIFY(!manager.externalAppInstalled());
        manager.refresh();QTRY_VERIFY(!manager.busy());
        QVERIFY(!manager.statusKnown());QVERIFY(!manager.serviceRunning());
        QVERIFY(!manager.errorMessage().isEmpty());QVERIFY(calls().isEmpty());
        qputenv("PATH",path);
        QSignalSpy installation(&manager,&TailscaleManager::externalStatusChanged);
        manager.refresh();QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.externalAppInstalled());QCOMPARE(installation.count(),1);
        QVERIFY(manager.statusKnown());QVERIFY(manager.loggedIn());
    }
    void savedDevicesRoundTripWithoutClaimingAnAccountOrFabricatingReachability() {
        TailscaleManager manager;
        QSignalSpy changed(&manager, &TailscaleManager::savedDevicesChanged);
        QVERIFY(manager.saveDevice({}, " Workstation ", " WORKSTATION.TAILNET.TS.NET. "));
        auto saved = manager.savedDevices(); QCOMPARE(saved.size(), 1);
        const auto first = saved.first().toMap();
        QCOMPARE(first.value("name").toString(), QString("Workstation"));
        QCOMPARE(first.value("ip").toString(), QString("workstation.tailnet.ts.net"));
        QVERIFY(first.value("included").toBool()); QVERIFY(first.value("saved").toBool());
        QVERIFY(!first.value("onlineKnown").toBool()); QVERIFY(!first.value("online").toBool());
        QVERIFY(!manager.usedByPilon()); QVERIFY(calls().isEmpty()); QCOMPARE(changed.count(), 1);
        const auto id = first.value("id").toString(); QVERIFY(id.startsWith("external:"));
        QVERIFY(manager.saveDevice(id, "Renamed", "[fd7a:115c:a1e0::1]"));
        TailscaleManager restarted;
        QCOMPARE(restarted.savedDevices().first().toMap().value("id").toString(), id);
        QCOMPARE(restarted.savedDevices().first().toMap().value("ip").toString(), QString("fd7a:115c:a1e0::1"));
        QVERIFY(manager.removeDevice(id)); QVERIFY(manager.savedDevices().isEmpty());
        QVERIFY(!manager.removeDevice(id)); QVERIFY(!manager.errorMessage().isEmpty());
    }
    void savedDeviceEditPreservesExclusionAndRejectsDuplicateAddresses() {
        TailscaleManager manager;
        QVERIFY(manager.saveDevice({}, "First", "100.64.0.1"));
        auto array = QJsonDocument::fromJson(PilonSettings().value("tailscale/savedDevices").toByteArray()).array();
        auto first = array.first().toObject(); first.insert("included", false); array[0] = first;
        PilonSettings().setValue("tailscale/savedDevices", QJsonDocument(array).toJson());
        QVERIFY(manager.saveDevice(first.value("id").toString(), "Edited", "100.64.0.2"));
        QVERIFY(!manager.savedDevices().first().toMap().value("included").toBool());
        QVERIFY(!manager.saveDevice({}, "Duplicate", "100.64.0.2"));
        QVERIFY(manager.saveDevice({}, "DNS", "host.example"));
        QVERIFY(!manager.saveDevice({}, "Same DNS", "HOST.EXAMPLE."));
        QCOMPARE(manager.savedDevices().size(), 2);
    }
    void invalidSavedDeviceInputNeverChangesStoredHosts_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<QString>("address");
        QTest::newRow("blank-name") << " " << "100.64.0.1";
        QTest::newRow("control-name") << QString("name\nline") << "host.example";
        QTest::newRow("blank-address") << "Device" << " ";
        QTest::newRow("url") << "Device" << "https://host.example";
        QTest::newRow("port") << "Device" << "host.example:22";
        QTest::newRow("ipv4") << "Device" << "100.999.0.1";
        QTest::newRow("ipv6") << "Device" << "fd7a:::1";
        QTest::newRow("shell") << "Device" << "host;touch x";
        QTest::newRow("dns-label") << "Device" << "-host.example";
    }
    void invalidSavedDeviceInputNeverChangesStoredHosts() {
        QFETCH(QString, name); QFETCH(QString, address);
        TailscaleManager manager;
        QVERIFY(manager.saveDevice({}, "Existing", "100.64.0.1"));
        const auto before = PilonSettings().value("tailscale/savedDevices");
        QVERIFY(!manager.saveDevice({}, name, address));
        QCOMPARE(PilonSettings().value("tailscale/savedDevices"), before);
        QVERIFY(!manager.errorMessage().isEmpty());
    }
    void unreadableSavedDevicesArePreservedUntilRestored() {
        const QByteArray broken("[{not-json]");
        PilonSettings().setValue("tailscale/savedDevices", broken);
        TailscaleManager manager;
        QVERIFY(manager.savedDevices().isEmpty());
        QVERIFY(!manager.saveDevice({}, "Replacement", "100.64.0.1"));
        QVERIFY(!manager.removeDevice("external:missing"));
        QCOMPARE(PilonSettings().value("tailscale/savedDevices").toByteArray(), broken);
    }
    void failedSavedDevicePersistencePreservesThePreviousList() {
        TailscaleManager manager;
        QVERIFY(manager.saveDevice({}, "Existing", "100.64.0.1"));
        const auto before = PilonSettings().value("tailscale/savedDevices");
        const auto folder = QFileInfo(PilonSettings().fileName()).absolutePath();
        const auto permissions = QFileInfo(folder).permissions();
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        auto restore = qScopeGuard([&] { QFile::setPermissions(folder, permissions); });
        QVERIFY(!manager.saveDevice({}, "Unsaved", "100.64.0.2"));
        QCOMPARE(PilonSettings().value("tailscale/savedDevices"), before);
        QCOMPARE(manager.savedDevices().size(), 1);
    }
    void destructionStopsPendingStatusWithoutCallbacks() {
        QFile original(fixture.filePath("tailscale")); QVERIFY(original.open(QIODevice::ReadOnly));
        const auto script = original.readAll(); original.close();
        const auto restore = qScopeGuard([&] { write("tailscale", script); });
        QVERIFY(write("tailscale", R"(#!/bin/sh
printf '%s\n' "$*" >> "$PILON_DEVICE_TEST_CALLS"
sleep 2
cat "$PILON_DEVICE_TEST_STATUS"
)"));
        QTest::failOnWarning(QRegularExpression("QProcess: Destroyed while process.*"));
        auto manager = std::make_unique<TailscaleManager>();
        int errors = 0;
        connect(manager.get(), &TailscaleManager::errorMessageChanged, this, [&] { ++errors; });
        manager->refresh();
        QTRY_VERIFY(calls().contains("status"));
        QVERIFY(manager->busy());
        manager.reset();
        QCOMPARE(errors, 0);
    }
    void passiveStatusNeverClaimsOrLogsOutAnUnrelatedSystemSession() {
        TailscaleManager manager;manager.refresh();QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.statusKnown());QVERIFY(manager.loggedIn());QVERIFY(!manager.usedByPilon());QVERIFY(!manager.accountClaimed());
        QVERIFY(manager.includedDevices().isEmpty());manager.logoutUsedAccount();
        QVERIFY(!calls().contains("logout"));QVERIFY(manager.loggedIn());QVERIFY(manager.cleanupBlockedReason().isEmpty());
    }
    void claimedAccountRequiresFreshStatusAndClearsOnlyAfterLogout() {
        TailscaleManager manager;QVERIFY(!manager.useCurrentAccount());
        manager.refresh();QTRY_VERIFY(!manager.busy());QVERIFY(manager.useCurrentAccount());
        QVERIFY(manager.usedByPilon());QVERIFY(manager.accountClaimed());
        TailscaleManager passive;QVERIFY(!passive.cleanupBlockedReason().isEmpty());
        passive.logoutUsedAccount();QVERIFY(!calls().contains("logout"));QVERIFY(passive.usedByPilon());
        passive.refresh();QTRY_VERIFY(!passive.busy());QVERIFY(passive.cleanupBlockedReason().isEmpty());
        passive.logoutUsedAccount();QTRY_VERIFY(!passive.busy());
        QVERIFY(calls().contains("logout"));QVERIFY(!passive.usedByPilon());QVERIFY(!passive.loggedIn());
    }
    void aReplacedSystemSessionIsPreserved() {
        TailscaleManager manager;manager.refresh();QTRY_VERIFY(!manager.busy());QVERIFY(manager.useCurrentAccount());
        QVERIFY(status("different-account"));manager.refresh();QTRY_VERIFY(!manager.busy());
        QVERIFY(!manager.accountClaimed());manager.logoutUsedAccount();
        QTRY_VERIFY(!manager.busy());QVERIFY(!manager.usedByPilon());QVERIFY(manager.loggedIn());QVERIFY(!calls().contains("logout"));
    }
    void legacyInclusionRequiresExplicitAccountConfirmation() {
        PilonSettings().setValue("tailscale/includedDevices",QStringList{"fixture-account"});
        TailscaleManager manager;manager.refresh();QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.usedByPilon());QVERIFY(!manager.accountClaimed());QVERIFY(!manager.cleanupBlockedReason().isEmpty());
        manager.logoutUsedAccount();QVERIFY(!calls().contains("logout"));
        QVERIFY(manager.useCurrentAccount());QVERIFY(manager.cleanupBlockedReason().isEmpty());
    }
    void unknownStatusCannotEraseAnOwnedAccountReceipt() {
        PilonSettings().setValue("tailscale/usedByPilon",true);
        QVERIFY(write("status","not JSON"));TailscaleManager manager;manager.refresh();QTRY_VERIFY(!manager.busy());
        QVERIFY(!manager.statusKnown());QVERIFY(!manager.cleanupBlockedReason().isEmpty());manager.logoutUsedAccount();
        QVERIFY(manager.usedByPilon());QVERIFY(!calls().contains("logout"));
    }
    void loginAndInclusionRefuseUnwritableAccountReceipts() {
        TailscaleManager manager;manager.refresh();QTRY_VERIFY(!manager.busy());
        PilonSettings().setValue("test/preflight",true);PilonSettings().sync();
        const auto folder=QFileInfo(PilonSettings().fileName()).absolutePath();
        const auto permissions=QFileInfo(folder).permissions();
        QVERIFY(QFile::setPermissions(folder,QFile::ReadOwner|QFile::ExeOwner));
        auto restore=qScopeGuard([&]{QFile::setPermissions(folder,permissions);PilonSettings().clear();});
        manager.login();QTRY_VERIFY(!manager.busy());
        QVERIFY(!calls().contains("login"));QVERIFY(!manager.errorMessage().isEmpty());
        QVERIFY(!manager.usedByPilon());
        manager.setDeviceIncluded("fixture-account",true);
        QVERIFY(manager.includedDevices().isEmpty());QVERIFY(!manager.usedByPilon());
    }
    void explicitDeviceInclusionAndLoginClaimTheObservedIdentity() {
        TailscaleManager manager;manager.refresh();QTRY_VERIFY(!manager.busy());
        manager.setDeviceIncluded("fixture-account",true);QVERIFY(manager.usedByPilon());QVERIFY(manager.accountClaimed());
        QVERIFY(!manager.includedDevices().isEmpty());
        PilonSettings().clear();manager.login();QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.usedByPilon());QVERIFY(manager.accountClaimed());
    }
};
QTEST_GUILESS_MAIN(DeviceTests)
#include "tailscale_tests.moc"
