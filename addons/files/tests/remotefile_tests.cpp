// Exercise the private traversal as well as its asynchronous UI lifecycle without
// adding test-only search paths or authentication bypasses to the application.
#include "../src/remotefilemanager.cpp"
#include <QPromise>
#include <filesystem>
#include <sys/inotify.h>
#include <QTcpServer>
#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest>

class RemoteFileTests : public QObject
{
    Q_OBJECT

    static QVariantMap device(const QString &id)
    {
        return {{"id", id}, {"name", id}, {"online", false}, {"ip", "127.0.0.1"}, {"username", "fixture"}};
    }

    static bool touch(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly);
    }

    static void finishSearch(RemoteFileManager &manager, const QVariantMap &result)
    {
        manager.m_operationWatcher.setFuture(QtFuture::makeReadyValueFuture(result));
    }

    static QVariantMap localEndpoint(const QString &path, bool directory = false)
    {
        return {{"deviceId", "local"}, {"username", pilonLocalDevice().value("username")},
            {"path", path}, {"name", QFileInfo(path).fileName()}, {"directory", directory}};
    }
    static bool writeVersion(const QString &path, const QByteArray &data, qint64 time)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size()
            && file.flush() && file.setFileTime(QDateTime::fromMSecsSinceEpoch(time), QFileDevice::FileModificationTime);
    }
    static QByteArray contents(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    }

private slots:
    void disablingSshStopsRemoteOperationsAndKeepsLocalFiles()
    {
        QTemporaryDir directory;
        QVERIFY(touch(directory.filePath("local-file")));
        RemoteFileManager manager;
        manager.browse(manager.localDevice(), {}, directory.path());
        QTRY_COMPARE(manager.currentPath(), directory.path());
        manager.setRemoteEnabled(false);
        manager.browse(device("peer"), "fixture", "/remote");
        QCOMPARE(manager.currentPath(), directory.path());
        QVERIFY(!manager.busy());
        QVERIFY(manager.errorMessage().contains("SSH add-on"));
        manager.createDirectory(manager.localDevice(), {}, directory.path(), "new-folder");
        QTRY_VERIFY(!manager.busy());
        QVERIFY(QFileInfo(directory.filePath("new-folder")).isDir());
    }

    void cachedRemoteListingIsScopedToAccountAndTrustedHost()
    {
        const auto peer = device("snapshot-test");
        const QString id = peer.value("id").toString(), account = "fixture", path = "/notes";
        const QByteArray key("fixture-host-key");
        PilonSettings settings;
        settings.setValue(trustedHostKeySetting(id), key.toBase64());
        settings.sync();
        const auto cleanup = qScopeGuard([&] {
            settings.remove(trustedHostKeySetting(id));
            NetworkSnapshot::clear("sftp");
        });
        const auto file = listingSnapshot(peer, account, path, key);
        QVERIFY(!file.isEmpty());
        RemoteFileManager first;
        first.setAutomaticRefreshEnabled(false);
        first.m_pendingRequest = {{"kind", "browse"}, {"snapshotPath", file}, {"path", path}};
        finishSearch(first, {{"status", "ok"}, {"device", peer}, {"username", account}, {"path", path},
            {"entries", QVariantList{QVariantMap{{"name", "draft.txt"}, {"path", "/notes/draft.txt"}}}}});
        QTRY_VERIFY(QFileInfo::exists(file));

        RemoteFileManager reopened;
        reopened.browse(peer, account, path);
        QTRY_COMPARE(reopened.currentPath(), path);
        QTRY_VERIFY(!reopened.busy());  // Fixture peer is offline.
        QCOMPARE(reopened.entries().first().toMap().value("name").toString(), QString("draft.txt"));
        QVERIFY(!reopened.errorMessage().isEmpty());
        QVERIFY(NetworkSnapshot::read(listingSnapshot(peer, "other", path, key)).isEmpty());
        QVERIFY(NetworkSnapshot::read(listingSnapshot(peer, account, path, "another-key")).isEmpty());
        QVERIFY(reopened.cachedListing());
        reopened.createDirectory(peer, account, path, "new-folder");
        QVERIFY(!reopened.busy());
        QVERIFY(reopened.errorMessage().contains("Refresh"));
        reopened.browse(peer, "other", path);
        QTRY_VERIFY(!reopened.busy());
        QVERIFY(reopened.entries().isEmpty());
        QVERIFY(!reopened.cachedListing());
        reopened.setRemoteEnabled(false);
        reopened.clearRemoteSnapshots();
        QVERIFY(!QFileInfo::exists(file));
        QTRY_VERIFY(!reopened.busy());
        QCOMPARE(reopened.currentDeviceId(), QString("local"));
    }

    void rejectedSftpAccessClearsLiveAndCachedListings_data()
    {
        QTest::addColumn<QString>("status");
        QTest::addColumn<bool>("cached");
        QTest::newRow("live rejected-account") << QString("credential") << false;
        QTest::newRow("cached unknown-key") << QString("hostKey") << true;
        QTest::newRow("live changed-key") << QString("error") << false;
    }

    void rejectedSftpAccessClearsLiveAndCachedListings()
    {
        QFETCH(QString, status);
        QFETCH(bool, cached);
        const auto peer = device("denied-peer");
        const QString account = "fixture", path = "/home/fixture";
        const QByteArray key("accepted-key");
        const auto file = listingSnapshot(peer, account, path, key);
        const auto alias = listingSnapshot(peer, account, ".", key);
        const auto home = listingSnapshot(peer, account, {}, key);
        const auto otherHost = listingSnapshot(device("unrelated"), account, path, key);
        const auto otherAccount = listingSnapshot(peer, "another", path, key);
        const auto cleanup = qScopeGuard([&] {
            for (const auto &name : {file, alias, home, otherHost, otherAccount}) QFile::remove(name);
        });
        for (const auto &name : {file, alias, home, otherHost, otherAccount}) {
            NetworkSnapshot::write(name, {{"path", path}, {"entries", QJsonArray{}}});
            QVERIFY(QFile::exists(name));
        }
        RemoteFileManager manager;
        manager.setAutomaticRefreshEnabled(false);
        const QVariantList rows{QVariantMap{{"name", "private"}, {"path", path + "/private"}}};
        manager.showListing(peer, account, path, rows, cached);
        RemoteFileManager otherWindow;
        otherWindow.showListing(peer, account, path, rows, !cached);
        RemoteFileManager unaffected;
        unaffected.showListing(device("unrelated"), account, path, rows, true);
        QVERIFY(!manager.entries().isEmpty());
        manager.m_pendingRequest = manager.m_listingRequest;
        manager.m_pendingRequest.insert("snapshotPath", file);
        manager.m_operationWatcher.setFuture(QtConcurrent::run([=] {
            if (status != "credential")
                return verifySftpHostKey(peer, status == "hostKey" ? QByteArray{} : key, "new-host-key");
            return rejectSftpAccess(peer, account, {{"status", status}, {"deviceId", peer.value("id")},
                {"username", account}, {"prompt", "Authorization required"}});
        }));
        if (status == "error") QTRY_VERIFY(manager.errorMessage().contains("has changed"));
        else QTRY_COMPARE(manager.promptKind(), status);
        QTRY_VERIFY(manager.entries().isEmpty());
        QVERIFY(manager.currentDeviceId().isEmpty());
        QVERIFY(!manager.cachedListing());
        QVERIFY(!QFile::exists(file));
        QVERIFY(!QFile::exists(alias));
        QVERIFY(!QFile::exists(home));
        QVERIFY(otherWindow.entries().isEmpty());
        QCOMPARE(unaffected.entries().size(), 1);
        QVERIFY(QFile::exists(otherHost));
        QCOMPARE(QFile::exists(otherAccount), status == "credential");
    }

    void unavailableSftpFolderClearsOnlyItsSubtreeAndAliases()
    {
        const auto peer = device("denied-folder-peer");
        const QString account = "fixture", canonical = "/home/fixture";
        const QByteArray key("accepted-key");
        const auto alias = listingSnapshot(peer, account, {}, key);
        const auto folder = listingSnapshot(peer, account, canonical, key);
        const auto nested = listingSnapshot(peer, account, canonical + "/notes", key);
        const auto sibling = listingSnapshot(peer, account, canonical + "-public", key);
        const auto cleanup = qScopeGuard([&] {
            for (const auto &file : {alias, folder, nested, sibling}) QFile::remove(file);
        });
        NetworkSnapshot::write(alias, {{"path", canonical}});
        NetworkSnapshot::write(folder, {{"path", canonical}});
        NetworkSnapshot::write(nested, {{"path", canonical + "/notes"}});
        NetworkSnapshot::write(sibling, {{"path", canonical + "-public"}});
        RemoteFileManager child, other;
        child.showListing(peer, account, canonical + "/notes", {}, true);
        other.showListing(peer, account, canonical + "-public", {}, false);
        for (const auto code : {LIBSSH2_FX_PERMISSION_DENIED, LIBSSH2_FX_NO_SUCH_FILE, LIBSSH2_FX_NO_SUCH_PATH})
            QVERIFY(sftpFolderUnavailable(LIBSSH2_ERROR_SFTP_PROTOCOL, code));
        QVERIFY(!sftpFolderUnavailable(LIBSSH2_ERROR_SOCKET_RECV, LIBSSH2_FX_PERMISSION_DENIED));
        QVERIFY(!sftpFolderUnavailable(LIBSSH2_ERROR_SFTP_PROTOCOL, LIBSSH2_FX_CONNECTION_LOST));
        rejectSftpDirectory(peer, account, {}, {}, key);
        QVERIFY(!QFile::exists(alias));
        QVERIFY(!QFile::exists(folder));
        QVERIFY(!QFile::exists(nested));
        QVERIFY(QFile::exists(sibling));
        QVERIFY(child.currentPath().isEmpty());
        QCOMPARE(other.currentPath(), canonical + "-public");
    }

    void paintedDragPreviewFollowsTheOriginalGrabPoint()
    {
        QQuickWindow window;
        window.resize(500, 500);
        QPixmap pixmap(224, 216);
        pixmap.setDevicePixelRatio(2);
        pixmap.fill(Qt::transparent);
        FileDragPreview preview(&window, pixmap, QPointF(59, 27));
        QCOMPARE(preview.size(), QSizeF(112, 108));
        QMimeData data;
        QDragEnterEvent enter(QPointF(139, 127), Qt::MoveAction, &data, Qt::LeftButton, {});
        QCoreApplication::sendEvent(&window, &enter);
        QVERIFY(preview.isVisible());
        QCOMPARE(preview.position(), QPointF(80, 100));
        QDragMoveEvent move(QPointF(200.5, 161.5), Qt::MoveAction, &data, Qt::LeftButton, {});
        QCoreApplication::sendEvent(&window, &move);
        QCOMPARE(preview.position(), QPointF(141.5, 134.5));
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(&window, &leave);
        QVERIFY(!preview.isVisible());
    }

    void disablingSshDiscardsAPendingRemoteBrowseAndRestoresTheLocalFolder()
    {
        QTemporaryDir directory;
        QVERIFY(touch(directory.filePath("keep.txt")));
        RemoteFileManager manager;
        manager.browse(manager.localDevice(), {}, directory.path());
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.currentPath(), directory.path());

        const auto peer = device("peer");
        const QVariantMap remoteBrowse{{"kind", "browse"}, {"devices", QVariantList{peer}}, {"path", "/private"}};
        manager.m_listingRequest = remoteBrowse;
        manager.m_pendingRequest = remoteBrowse;
        manager.m_cancelRequested = std::make_shared<std::atomic_bool>(false);
        QPromise<QVariantMap> pending;
        pending.start();
        manager.m_operationWatcher.setFuture(pending.future());
        manager.setBusy(true);
        manager.setRemoteEnabled(false);
        QVERIFY(manager.m_cancelRequested->load());
        pending.addResult(QVariantMap{{"status", "ok"}, {"device", peer}, {"path", "/private"}});
        pending.finish();
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.currentDeviceId(), QString("local"));
        QCOMPARE(manager.currentPath(), directory.path());
        QCOMPARE(manager.entries().value(0).toMap().value("name").toString(), QString("keep.txt"));
    }

    void onlyVerifiedCredentialsEnterTheSharedCache()
    {
        RemoteFileManager manager;
        const auto peer = device("peer");
        const auto host = peer.value("ip").toString();
        const QString username = "unused-fixture-user";
        const QVariantMap request{{"kind", "browse"}, {"devices", QVariantList{peer}}, {"username", username}};
        manager.m_pendingRequest = request;
        manager.m_promptDeviceId = "peer";
        manager.m_promptUsername = username;
        manager.setPrompt("credential", "Password");
        manager.submitCredential("fixture-only");
        QVERIFY(SshCredentials::get(host, username).isEmpty());
        QTRY_VERIFY(!manager.busy());
        QVERIFY(SshCredentials::get(host, username).isEmpty());
        manager.m_pendingRequest = request;
        manager.m_activeCredential = {"peer", host, username, "fixture-only", SshCredentials::revision(), false};
        finishSearch(manager, {{"status", "error"}, {"error", "Unable to open the folder"}, {"credentialAccepted", true}});
        QTRY_COMPARE(SshCredentials::get(host, username), QString("fixture-only"));
        SshCredentials::put(host, username, {});
    }

    void savedHostReachabilityRemainsUnknownAndActionable()
    {
        const QVariantMap entry{{"deviceId", "external:fixture"}, {"deviceName", "Saved host"},
            {"ip", ""}, {"online", false}, {"onlineKnown", false}};
        auto target = deviceFromEntry(entry);
        QVERIFY(target.contains("onlineKnown"));
        QVERIFY(!target.value("onlineKnown").toBool());
        SftpConnection connection;
        auto result = authenticate(connection, target, "fixture", {}, false, {}, {});
        QVERIFY(result.value("error").toString().contains("no usable address"));
        target.insert("onlineKnown", true);
        result = authenticate(connection, target, "fixture", {}, false, {}, {});
        QVERIFY(result.value("error").toString().contains("unavailable"));
        target.remove("onlineKnown");
        result = authenticate(connection, target, "fixture", {}, false, {}, {});
        QVERIFY(result.value("error").toString().contains("unavailable"));
    }
    void retiredLinkApisAreAbsentAndCannotStartWork()
    {
        RemoteFileManager manager;
        QCOMPARE(manager.metaObject()->indexOfProperty("legacyLinks"), -1);
        for (const auto *name : {"setLink(QVariantMap,QVariantMap,QString)", "refreshLinkRecords(QVariantList,bool)",
                                 "updateFromLink(QString,QVariantMap,QVariantList)", "removeLinkById(QString)"})
            QCOMPARE(manager.metaObject()->indexOfMethod(name), -1);
        QSignalSpy busy(&manager, &RemoteFileManager::busyChanged);
        QTest::qWait(200);
        QVERIFY(!manager.busy());
        QVERIFY(manager.m_pendingRequest.isEmpty());
        QVERIFY(!manager.m_operationWatcher.isRunning());
        QCOMPARE(busy.count(), 0);
    }
    void automaticRefreshCanPauseWithoutDisablingExplicitBrowsing()
    {
        QTemporaryDir directory;
        QVERIFY(touch(directory.filePath("initial")));
        RemoteFileManager manager;
        const auto local = manager.localDevice();
        const auto username = local.value("username").toString();
        manager.browse(local, username, directory.path());
        QTRY_VERIFY(!manager.busy());
        QCOMPARE(manager.entries().size(), 1);
        QVERIFY(manager.m_fileWatcher.directories().contains(directory.path()));
        QVERIFY(touch(directory.filePath("queued-refresh")));
        QTRY_VERIFY(manager.m_fileRefreshTimer.isActive());
        QVERIFY(manager.setProperty("automaticRefreshEnabled", false));
        QVERIFY(!manager.m_fileRefreshTimer.isActive());
        QVERIFY(manager.m_fileWatcher.directories().isEmpty());
        QVERIFY(manager.m_fileWatcher.files().isEmpty());
        const auto listing = manager.m_listingRequest;
        QVERIFY(touch(directory.filePath("while-paused")));
        QTest::qWait(350);
        QVERIFY(!manager.busy()); QCOMPARE(manager.entries().size(), 1);
        QCOMPARE(manager.currentPath(), directory.path()); QCOMPARE(manager.m_listingRequest, listing);
        manager.browse(local, username, directory.path());
        QTRY_VERIFY(!manager.busy()); QCOMPARE(manager.entries().size(), 3);
        QVERIFY(manager.m_fileWatcher.directories().isEmpty());
        QVERIFY(touch(directory.filePath("still-paused")));
        QTest::qWait(350); QCOMPARE(manager.entries().size(), 3);
        QVERIFY(manager.setProperty("automaticRefreshEnabled", true));
        QVERIFY(manager.m_fileWatcher.directories().contains(directory.path()));
        QVERIFY(touch(directory.filePath("after-resume")));
        QTRY_COMPARE(manager.entries().size(), 5);
        QTRY_VERIFY(!manager.busy());
    }

    void stalledSshHandshakeCanBeCancelledAndDestroyed_data()
    {
        QTest::addColumn<bool>("destroy");
        QTest::addColumn<bool>("blocking");
        QTest::newRow("cancel") << false << false;
        QTest::newRow("destroy") << true << false;
        QTest::newRow("cancel-blocked-network") << false << true;
        QTest::newRow("destroy-blocked-network") << true << true;
    }
    void stalledSshHandshakeCanBeCancelledAndDestroyed()
    {
        QFETCH(bool, destroy);
        QFETCH(bool, blocking);
        QCOMPARE(libssh2_init(0), 0);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto manager = std::make_unique<RemoteFileManager>();
        manager->m_pendingRequest = {{"kind", "browse"}};
        manager->m_cancelRequested = std::make_shared<std::atomic_bool>(false);
        manager->m_transfer = std::make_shared<FileTransferProgress>();
        manager->setBusy(true);
        manager->m_operationWatcher.setFuture(QtConcurrent::run(
            [port = server.serverPort(), cancel = manager->m_cancelRequested, progress = manager->m_transfer, blocking] {
                auto *previous = std::exchange(transferProgress, progress.get());
                auto reset = qScopeGuard([&] { transferProgress = previous; });
                SftpConnection connection;
                connection.socket.connectToHost(QHostAddress::LocalHost, port);
                if (!connection.socket.waitForConnected(1000)) return failure("Fixture connection failed");
                connection.registerSocket();
                connection.session = libssh2_session_init();
                libssh2_session_set_timeout(connection.session, connectionTimeoutMs);
                libssh2_session_set_blocking(connection.session, blocking);
                const auto handshake = [&] {
                    return libssh2_session_handshake(connection.session, connection.socket.socketDescriptor());
                };
                const int result = blocking ? handshake() : connection.call(handshake, cancel);
                return QVariantMap{{"status", cancel->load() ? "cancelled" : "error"}, {"code", result}};
            }));
        QTRY_VERIFY(server.hasPendingConnections());
        auto *socket = server.nextPendingConnection();
        QTRY_VERIFY(socket->bytesAvailable() > 0);
        QVERIFY(socket->readAll().startsWith("SSH-2.0-libssh2"));
        // The peer never sends a banner, so authentication cannot make progress.
        QElapsedTimer elapsed;
        elapsed.start();
        if (destroy) manager.reset();
        else {
            manager->cancel();
            QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), 1000);
            QCOMPARE(manager->m_operationWatcher.result().value("status"), QString("cancelled"));
            QVERIFY(manager->promptKind().isEmpty());
        }
        QVERIFY2(elapsed.elapsed() < 1000, "Cancelling or closing must not block on an SSH peer");
    }

    void directoryTransfersReportOneTotalWithoutResettingForEachFile()
    {
        QTemporaryDir root;
        const auto source = root.filePath("source"), destination = root.filePath("destination");
        QVERIFY(QDir().mkpath(source + "/nested"));
        QVERIFY(writeVersion(source + "/one", QByteArray(100, 'a'), 1000));
        QVERIFY(writeVersion(source + "/nested/two", QByteArray(300, 'b'), 1000));
        QVERIFY(QFile::link("missing-target", source + "/link"));
        RemoteFileManager manager;
        manager.m_transfer = std::make_shared<FileTransferProgress>();
        auto *previous = std::exchange(transferProgress, manager.m_transfer.get());
        auto reset = qScopeGuard([&] { transferProgress = previous; });
        SftpConnection local; local.local = true;
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        QCOMPARE(transferSize(local, source, true, cancel), 400);
        prepareTransfer(400, source);
        QCOMPARE(copyRemotePath(local, local, source, destination, true, cancel).value("status"), QString("ok"));
        const auto progress = manager.transfer();
        QCOMPARE(progress.value("sent").toLongLong(), 400);
        QCOMPARE(progress.value("total").toLongLong(), 400);
        QVERIFY(progress.value("totalKnown").toBool());
        QCOMPARE(contents(destination + "/one"), QByteArray(100, 'a'));
        QCOMPARE(contents(destination + "/nested/two"), QByteArray(300, 'b'));
    }

    void renameChangesOnlyTheNameAndRejectsDestinationPaths()
    {
        QTemporaryDir root;
        QVERIFY(writeVersion(root.filePath("before"), "content", 1000));
        RemoteFileManager manager;
        const auto entry = localEndpoint(root.filePath("before"));
        manager.renameEntry(entry, {}, "other/name");
        QVERIFY(!manager.errorMessage().isEmpty());
        QVERIFY(QFileInfo::exists(root.filePath("before")));
        manager.renameEntry(entry, {}, "after");
        QTRY_VERIFY(!manager.busy());
        QVERIFY(!QFileInfo::exists(root.filePath("before")));
        QCOMPARE(contents(root.filePath("after")), QByteArray("content"));
    }

    void movingEntriesOnTheSameDeviceCompletesTheWholeProgress()
    {
        QTemporaryDir root;
        const auto source = root.filePath("source"), destination = root.filePath("destination");
        QVERIFY(QDir().mkpath(source + "/nested"));
        QVERIFY(QDir().mkpath(destination));
        QVERIFY(writeVersion(source + "/nested/one", QByteArray(100, 'a'), 1000));
        QVERIFY(writeVersion(root.filePath("two"), QByteArray(300, 'b'), 1000));
        RemoteFileManager manager;
        manager.m_transfer = std::make_shared<FileTransferProgress>();
        auto *previous = std::exchange(transferProgress, manager.m_transfer.get());
        auto reset = qScopeGuard([&] { transferProgress = previous; });
        auto folder = localEndpoint(source); folder["directory"] = true;
        const auto device = manager.localDevice();
        const auto result = executeRequest({{"kind", "paste"}, {"username", ""}, {"devices", QVariantList{device}},
            {"entries", QVariantList{folder, localEndpoint(root.filePath("two"))}}, {"destinationDevice", device},
            {"destinationDirectory", destination}, {"move", true}}, std::make_shared<std::atomic_bool>(false));
        QCOMPARE(result.value("status"), QString("ok"));
        QCOMPARE(manager.transfer().value("sent").toLongLong(), 400);
        QCOMPARE(manager.transfer().value("total").toLongLong(), 400);
        QVERIFY(!QFileInfo::exists(source));
        QCOMPARE(contents(destination + "/source/nested/one"), QByteArray(100, 'a'));
        QCOMPARE(contents(destination + "/two"), QByteArray(300, 'b'));
    }

    void archivesRoundTripAndRejectTraversalAndOverwrites() {
        QTemporaryDir directory; const auto folder = directory.filePath("folder"); QVERIFY(QDir().mkpath(folder));
        QVERIFY(writeVersion(folder + "/file.txt", "archive evidence", 10000));
        QVERIFY(QFile::setPermissions(folder + "/file.txt", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        RemoteFileManager manager;
        manager.archiveEntry(localEndpoint(folder, true), "", false); QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
        QVERIFY2(manager.errorMessage().isEmpty(), qPrintable(manager.errorMessage()));
        QVERIFY(QFileInfo::exists(folder + ".zip"));
        manager.archiveEntry(localEndpoint(folder + ".zip"), "", true); QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
        QVERIFY2(manager.errorMessage().isEmpty(), qPrintable(manager.errorMessage()));
        QFile extracted(folder + ".zip.extracted/folder/file.txt"); QVERIFY(extracted.open(QIODevice::ReadOnly)); QCOMPARE(extracted.readAll(), QByteArray("archive evidence"));
        QVERIFY(extracted.permissions() & QFile::ExeOwner);
        manager.archiveEntry(localEndpoint(folder + ".zip"), "", true); QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
        QVERIFY(manager.errorMessage().contains("already exists"));
        const auto unsafe = directory.filePath("unsafe.zip");
        QProcess python; python.start("python3", {"-c", "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1],'w'); z.writestr('../escape.txt','bad'); z.close()", unsafe});
        QVERIFY(python.waitForFinished()); QCOMPARE(python.exitCode(), 0);
        manager.archiveEntry(localEndpoint(unsafe), "", true); QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
        QVERIFY(manager.errorMessage().contains("unsafe")); QVERIFY(!QFileInfo::exists(directory.filePath("escape.txt")));
        QVERIFY(!QFileInfo::exists(unsafe + ".extracted"));
    }
    void audioPreviewDecodesWaveformAndClearsIt() {
        QTemporaryDir directory;
        const auto file = directory.filePath("tone.mp3");
        QProcess ffmpeg;
        ffmpeg.start("ffmpeg", {"-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=2", file});
        QVERIFY(ffmpeg.waitForFinished(10000)); QCOMPARE(ffmpeg.exitCode(), 0);
        RemoteFileManager manager; manager.viewFile(localEndpoint(file), "");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 10000);
        QCOMPARE(manager.previewKind(), QString("audio"));
        const auto waveform = manager.property("previewWaveform").toList();
        QCOMPARE(waveform.size(), 48);
        for (const auto &value : waveform) QVERIFY(value.toDouble() > 0 && value.toDouble() <= 1);
        manager.clearPreview(); QVERIFY(manager.property("previewWaveform").toList().isEmpty());
    }
    void nativeFilesOpenDirectlyWithoutPreview() {
        QTemporaryDir directory;
        RemoteFileManager manager;
        QSignalSpy opening(&manager, &RemoteFileManager::pilonFileOpening);
        QSignalSpy ready(&manager, &RemoteFileManager::pilonFileReady);
        for (const auto &name : {"Planning.pin", "Lecture.pical", "UPPER.PICAL", "Example.piext"}) {
            const auto path = directory.filePath(name);
            QVERIFY(writeVersion(path, "native file fixture", 10000));
            manager.viewFile(localEndpoint(path), "");
            QCOMPARE(ready.last().first().toUrl(), QUrl::fromLocalFile(path));
            QVERIFY(manager.previewName().isEmpty());
            QVERIFY(manager.previewKind().isEmpty());
            QVERIFY(manager.previewUrl().isEmpty());
            QVERIFY(!manager.previewLoading());
            QVERIFY(!manager.busy());
        }
        QCOMPARE(opening.size(), 4); QCOMPARE(ready.size(), 4);
        // Remote staging completes through the same native route, without rendering.
        finishSearch(manager, {{"status", "ok"}, {"operation", "openPilon"},
            {"file", QUrl::fromLocalFile(directory.filePath("Planning.pin"))}});
        QTRY_COMPARE(ready.size(), 5);
        QVERIFY(manager.previewKind().isEmpty());
        QVERIFY(manager.previewName().isEmpty());
    }

    void filePreviewSupportsImageTextAndDocuments() {
        QTemporaryDir directory; QImage image(40, 20, QImage::Format_RGB32); image.fill(Qt::blue);
        const auto path = directory.filePath("image.png"); QVERIFY(image.save(path));
        RemoteFileManager manager; manager.viewFile(localEndpoint(path), "");
        QCOMPARE(manager.previewName(), QString("image.png")); QVERIFY(manager.previewLoading());
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000); QVERIFY(!manager.previewLoading());
        QVERIFY2(manager.errorMessage().isEmpty(), qPrintable(manager.errorMessage())); QCOMPARE(manager.previewKind(), QString("image"));
        QCOMPARE(manager.previewUrl(), QUrl::fromLocalFile(path));
        const auto text = directory.filePath("table.csv"); QVERIFY(writeVersion(text, "Name,Size\nPilon,24\n", 10000));
        manager.viewFile(localEndpoint(text), ""); QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 10000);
        QVERIFY2(manager.errorMessage().isEmpty(), qPrintable(manager.errorMessage()));
        QVERIFY(manager.previewKind() == "text" || manager.previewKind() == "document");
        const auto document = QFINDTESTDATA("../../../src/tests/fixtures/table.xlsx"); QVERIFY(!document.isEmpty());
        if (!NoteImport::officeExecutable().isEmpty()) {
            manager.viewFile(localEndpoint(document), ""); QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 30000);
            QVERIFY2(manager.errorMessage().isEmpty(), qPrintable(manager.errorMessage()));
            QCOMPARE(manager.previewKind(), QString("document")); QVERIFY(manager.previewPages().isEmpty());
            QVERIFY(QFileInfo::exists(manager.property("previewDocumentUrl").toUrl().toLocalFile()));
        }
    }

    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("PilonTests");
        QCoreApplication::setApplicationName("FileTests");
    }

    void appearanceIconImportsAreValidatedAndCopiedIntoWorkspace()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        QSettings bootstrap(PilonWorkspace::bootstrapFile(), QSettings::IniFormat);
        const auto oldRoot = bootstrap.value("workspace/root");
        const auto oldVersion = bootstrap.value("workspace/version");
        const auto restore = qScopeGuard([&] {
            if (oldRoot.isValid()) bootstrap.setValue("workspace/root", oldRoot);
            else bootstrap.remove("workspace/root");
            if (oldVersion.isValid()) bootstrap.setValue("workspace/version", oldVersion);
            else bootstrap.remove("workspace/version");
            bootstrap.sync();
        });
        bootstrap.setValue("workspace/root", workspace.path());
        bootstrap.setValue("workspace/version", 1);
        bootstrap.sync();
        const auto data = PilonWorkspace::addonDataDirectory("files");
        QCOMPARE(data, workspace.filePath("addon-data/files"));
        QVERIFY(QDir().mkpath(data));

        RemoteFileManager manager;
        const auto png = workspace.filePath("source.png");
        QImage image(32, 24, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(png));
        const QUrl source = QUrl::fromLocalFile(png);
        QVERIFY(manager.importAppearanceIcon(source, workspace.path()).isEmpty());
        const auto first = manager.importAppearanceIcon(source, data);
        QVERIFY(!first.isEmpty());
        QCOMPARE(manager.importAppearanceIcon(source, data), first);
        QVERIFY(QFile::remove(png));
        const auto importedPng = manager.appearanceIconUrl(first).toLocalFile();
        QVERIFY(importedPng.startsWith(data + "/appearance-icons/"));
        QCOMPARE(QImage(importedPng).pixelColor(0, 0), QColor(Qt::red));

        const auto svg = workspace.filePath("source.svg");
        const QByteArray svgContents =
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="200"><rect width="400" height="200" fill="blue"/></svg>)";
        QVERIFY(writeVersion(svg, svgContents, 1500));
        const auto second = manager.importAppearanceIcon(QUrl::fromLocalFile(svg), data);
        QVERIFY(!second.isEmpty());
        const QImage importedSvg(manager.appearanceIconUrl(second).toLocalFile());
        QCOMPARE(importedSvg.size(), QSize(256, 128));
        QCOMPARE(importedSvg.pixelColor(0, 0), QColor(Qt::blue));
        QVERIFY(QFile::remove(manager.appearanceIconUrl(second).toLocalFile()));
        QVERIFY(manager.appearanceIconUrl(second).isEmpty());

        QVERIFY(writeVersion(svg, "invalid", 1600));
        QVERIFY(manager.importAppearanceIcon(QUrl::fromLocalFile(svg), data).isEmpty());
        image = QImage(2049, 1, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(png));
        QVERIFY(manager.importAppearanceIcon(source, data).isEmpty());
        QVERIFY(manager.appearanceIconUrl("../source").isEmpty());
    }

    void sharedCredentialsSurviveClosingTheSavingWindow()
    {
        QTemporaryDir directory;
        const auto saved = directory.filePath("saved");
        const auto helper = directory.filePath("secret-tool");
        QFile script(helper); QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(("#!/bin/sh\ncat > " + shellQuote(saved) + "\n").toUtf8()); script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        {
            RemoteFileManager manager;
            manager.m_secretToolExecutable = helper;
            manager.m_pendingRequest = {{"devices", QVariantList{QVariantMap{{"id", "peer"}, {"ip", "credential-test-host"}}}}};
            manager.saveCredential("peer", "alice", "fixture-only");
        }
        QTRY_COMPARE(contents(saved), QByteArray("fixture-only"));
        QCOMPARE(SshCredentials::get("credential-test-host", "alice"), QString("fixture-only"));
        QVERIFY(SshCredentials::get("credential-test-host", "bob").isEmpty());
        QVERIFY(SshCredentials::get("another-host", "alice").isEmpty());
        SshCredentials::put("credential-test-host", "alice", {});
    }

    void folderSizesArriveAfterListingWithoutFollowingSymlinks()
    {
        QTemporaryDir root, outside;
        QVERIFY(QDir().mkpath(root.filePath("folder/sub")));
        QVERIFY(writeVersion(root.filePath("folder/sub/file"), "1234567", 1000));
        QVERIFY(writeVersion(outside.filePath("outside"), QByteArray(500000, 'x'), 1000));
        QVERIFY(QFile::link(outside.path(), root.filePath("folder/alias")));
        RemoteFileManager manager;
        manager.browse(manager.localDevice(), {}, root.path());
        QTRY_VERIFY(!manager.busy());
        QTRY_VERIFY(manager.entries().first().toMap().value("size").isValid());
        const auto size = manager.entries().first().toMap().value("size").toLongLong();
        QVERIFY(size >= 7); QVERIFY(size < 500000);
    }

    void watchedSizesSkipUnchangedSubtreesAndSeeInPlaceFileResizes()
    {
        QTemporaryDir root("/dev/shm/pilon-size-XXXXXX");
        if (!root.isValid()) QSKIP("This test requires a writable tmpfs");
        const auto changed = root.filePath("changed/deep");
        const auto stable = root.filePath("stable");
        QVERIFY(QDir().mkpath(changed));
        QVERIFY(QDir().mkpath(stable));
        QVERIFY(writeVersion(changed + "/data", "1234", 1000));
        for (int i = 0; i < 128; ++i)
            QVERIFY(writeVersion(stable + '/' + QString::number(i), "unchanged", 1000));
        const int observer = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        QVERIFY(observer >= 0);
        const auto close = qScopeGuard([observer] { ::close(observer); });
        QVERIFY(::inotify_add_watch(observer, QFile::encodeName(stable).constData(), IN_OPEN) >= 0);
        const auto openedStable = [&] {
            alignas(inotify_event) char buffer[4096];
            return ::read(observer, buffer, sizeof(buffer)) > 0;
        };

        FolderSizeCache cache;
        std::atomic_bool cancel{false};
        bool watched = false;
        const auto first = cache.measure(root.path(), cancel, &watched);
        QVERIFY(first.has_value());
        QVERIFY(watched);
        QVERIFY(openedStable());
        QCOMPARE(cache.measure(root.path(), cancel), first);
        QVERIFY(!openedStable());
        const auto directoryTime = QFileInfo(changed).lastModified();
        QVERIFY(writeVersion(changed + "/data", "123456789", 1000));
        QCOMPARE(QFileInfo(changed).lastModified(), directoryTime);
        QCOMPARE(cache.measure(root.path(), cancel), std::optional<qint64>(*first + 5));
        QVERIFY(!openedStable());
        cancel.store(true);
        QVERIFY(!cache.measure(root.path(), cancel));
    }

    void storedSizeEstimatesSurviveReopeningAndRefreshFromDisk()
    {
        QTemporaryDir root;
        QVERIFY(QDir().mkpath(root.filePath("box")));
        QVERIFY(writeVersion(root.filePath("box/example"), "input", 1000));
        const auto local = pilonLocalDevice();
        const auto file = sizeSnapshotPath(local, {}, root.path(), {});
        const auto cleanup = qScopeGuard([&] { QFile::remove(file); });
        qint64 exact = 0;
        {
            RemoteFileManager first;
            first.browse(local, {}, root.path());
            QTRY_VERIFY(!NetworkSnapshot::read(file).isEmpty());
            exact = NetworkSnapshot::read(file).value("folders").toObject().value("box").toObject().value("bytes").toInteger();
            QVERIFY(exact >= 5);
        }
        // Simulate a previous process measuring a volume with no reliable notifications.
        auto snapshot = NetworkSnapshot::read(file);
        auto folders = snapshot.value("folders").toObject();
        auto record = folders.value("box").toObject();
        record.insert("bytes", 1);
        record.insert("watched", false);
        folders.insert("box", record);
        snapshot.insert("folders", folders);
        NetworkSnapshot::write(file, snapshot);
        RemoteFileManager reopened;
        reopened.browse(local, {}, root.path());
        QTRY_COMPARE(reopened.currentPath(), root.path());
        QTRY_VERIFY(!reopened.m_sizeWatcher.isRunning());
        QCOMPARE(reopened.entries().first().toMap().value("size").toLongLong(), 1);
        QVERIFY(reopened.entries().first().toMap().value("sizeCached").toBool());
        reopened.refresh();
        QTRY_VERIFY(!reopened.entries().first().toMap().value("sizeCached").toBool());
        QCOMPARE(reopened.entries().first().toMap().value("size").toLongLong(), exact);
        const auto now = record.value("measured").toInteger();
        const QVariantMap same{{"modified", record.value("modified").toInteger()}};
        QVERIFY(reusableSizeEstimate(record, same, now, false));
        QVERIFY(!reusableSizeEstimate(record, same, now + sizeEstimateLifetime, false));
        QVERIFY(!reusableSizeEstimate(record, same, now - 1, false));
        QVERIFY(!reusableSizeEstimate(record, same, now, true));
        QVERIFY(!reusableSizeEstimate(record, {{"modified", -1}}, now, false));
    }

    void rejectedSftpAccountsDiscardTheirFolderSizeEstimates()
    {
        const auto peer = device("size-estimate-test");
        const auto first = sizeSnapshotPath(peer, "first", "/notes", "server-key");
        const auto second = sizeSnapshotPath(peer, "second", "/notes", "server-key");
        const auto cleanup = qScopeGuard([&] { QFile::remove(first); QFile::remove(second); });
        QVERIFY(!first.isEmpty());
        QVERIFY(first != second);
        QVERIFY(first != sizeSnapshotPath(peer, "first", "/notes", "other-key"));
        NetworkSnapshot::write(first, {{"private", true}});
        NetworkSnapshot::write(second, {{"private", true}});
        rejectSftpAccess(peer, "first", {});
        QVERIFY(!QFileInfo::exists(first));
        QVERIFY(QFileInfo::exists(second));
        rejectSftpAccess(peer, {}, {});
        QVERIFY(!QFileInfo::exists(second));
    }

    void localMutationsPreserveSiblingAndRemoteSizeSnapshots()
    {
        QTemporaryDir root;
        QVERIFY(QDir().mkpath(root.filePath("inside/nested")));
        QVERIFY(QDir().mkpath(root.filePath("stable")));
        const auto local = pilonLocalDevice();
        const auto child = root.filePath("inside/nested");
        const auto rootRecord = sizeSnapshotPath(local, {}, root.path(), {});
        const auto childRecord = sizeSnapshotPath(local, {}, child, {});
        const auto remote = sizeSnapshotPath(device("independent-size-test"), "user", root.path(), "key");
        const auto cleanup = qScopeGuard([&] {
            for (const auto &file : {rootRecord, childRecord, remote}) QFile::remove(file);
        });
        const QJsonObject stable{{"bytes", 72}};
        const QJsonObject folders{{"inside", QJsonObject{{"bytes", 42}}}, {"stable", stable}};
        NetworkSnapshot::write(rootRecord, {{"path", root.path()}, {"folders", folders}});
        NetworkSnapshot::write(childRecord, {{"path", child}, {"folders", folders}});
        NetworkSnapshot::write(remote, {{"path", root.path()}, {"folders", folders}});
        const auto result = executeRequest({{"kind", "deleteMany"}, {"devices", QVariantList{local}},
            {"entries", QVariantList{localEndpoint(child, true)}}}, std::make_shared<std::atomic_bool>(false));
        QCOMPARE(result.value("status"), "ok");
        const QJsonObject expected{{"stable", stable}};
        QCOMPARE(NetworkSnapshot::read(rootRecord).value("folders").toObject(), expected);
        QVERIFY(!QFile::exists(childRecord));
        QCOMPARE(NetworkSnapshot::read(remote).value("folders").toObject(), folders);
    }

    void remoteSizesUseRecentEstimatesAndFinishWhenTheServerIsOffline()
    {
        const auto peer = device("offline-size-test");
        const auto file = sizeSnapshotPath(peer, "fixture", "/home", "key");
        const auto cleanup = qScopeGuard([&] { QFile::remove(file); });
        const auto now = QDateTime::currentSecsSinceEpoch();
        const QJsonObject record{{"bytes", 100}, {"modified", 73}, {"measured", now}, {"watched", false}};
        const QJsonObject saved{{"current", record}, {"removed", record}};
        NetworkSnapshot::write(file, {{"path", "/home"}, {"folders", saved}});
        const QVariantMap folder{{"name", "current"}, {"path", "/home/current"}, {"directory", true}, {"modified", 73}};
        const QVariantMap newlyListed{{"name", "new"}, {"path", "/home/new"}, {"directory", true}, {"modified", 19}};
        const QVariantList entries{folder, newlyListed};
        RemoteFileManager manager;
        manager.m_entries = entries;
        restoreFolderSizes(manager.m_entries, saved);
        manager.m_sizeRequest = {{"device", peer}, {"username", "fixture"}, {"trustedKey", "key"},
            {"entries", entries}, {"path", "/home"}, {"sizeSnapshot", saved}, {"sizeEpoch", sizeEpoch(peer)},
            {"generation", manager.m_requestGeneration}};
        manager.startFolderSizes();
        QTRY_VERIFY(manager.m_entries.at(1).toMap().value("sizeUnavailable").toBool());
        QVERIFY(manager.m_entries.at(0).toMap().value("sizeCached").toBool());
        QCOMPARE(manager.m_entries.at(0).toMap().value("size").toLongLong(), 100);
        QTRY_COMPARE(NetworkSnapshot::read(file).value("folders").toObject().size(), 1);
    }

    void confirmedLocalDownloadReplacesAtomicallyAndCancellationPreservesDestination()
    {
        QTemporaryDir directory;
        const auto source = directory.filePath("source"), destination = directory.filePath("target");
        QVERIFY(writeVersion(source, "new contents", 1000));
        QVERIFY(writeVersion(destination, "existing", 1000));
        SftpConnection local; local.local = true;
        auto cancel = std::make_shared<std::atomic_bool>(true);
        QCOMPARE(downloadRemoteFile(local, source, destination, cancel).value("status"), QString("cancelled"));
        QCOMPARE(contents(destination), QByteArray("existing"));
        cancel->store(false);
        QCOMPARE(downloadRemoteFile(local, source, destination, cancel).value("status"), QString("ok"));
        QCOMPARE(contents(destination), QByteArray("new contents"));
    }

    void selectionStagesFilesAndTransferReportsBytes()
    {
        QTemporaryDir root;
        const auto source = root.filePath("input");
        const auto target = root.filePath("copy");
        QVERIFY(writeVersion(source, QByteArray(2 * 1024 * 1024 + 19, 'x'), 1000));
        FileTransferProgress progress;
        transferProgress = &progress;
        auto reset = qScopeGuard([] { transferProgress = nullptr; });
        const auto result = copyLocalFile(source, target, std::make_shared<std::atomic_bool>(false));
        QCOMPARE(result.value("status"), QString("ok"));
        QCOMPARE(progress.sent, QFileInfo(source).size());
        QCOMPARE(progress.total, progress.sent); QCOMPARE(progress.overall, progress.sent);
        QCOMPARE(contents(source), contents(target));
        RemoteFileManager manager;
        QSignalSpy ready(&manager, &RemoteFileManager::selectionReady);
        manager.prepareSelection({localEndpoint(source)}, {});
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(ready.first().first().toList().first().toUrl(), QUrl::fromLocalFile(source));
    }

    void hiddenEntriesCanBeToggledWithoutChangingTheListing()
    {
        RemoteFileManager manager;
        manager.m_entries = {localEndpoint("/home/user/visible", true), localEndpoint("/home/user/.config", true),
            localEndpoint("/home/user/.config/settings.json"), localEndpoint("/home/user/.secret")};
        QCOMPARE(manager.entries().size(), 4);
        manager.setShowHidden(false); QCOMPARE(manager.entries().size(), 2);
        manager.m_searchMode = true; QCOMPARE(manager.entries().size(), 1);
        manager.setShowHidden(true); QCOMPARE(manager.entries().size(), 4);
    }

    void discardedShareSelectionRemovesOnlyPrivateStaging()
    {
        RemoteFileManager manager;
        QTemporaryDir originals;
        const auto source = originals.filePath("share.txt");
        QVERIFY(touch(source));
        const auto folder = manager.m_previewDirectory.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QVERIFY(QDir().mkpath(folder + "/0"));
        const auto staged = folder + "/0/share.txt";
        QVERIFY(QFile::copy(source, staged));
        manager.discardPreparedSelection(QUrl::fromLocalFile(source));
        QVERIFY(QFile::exists(source));
        QVERIFY(QFile::exists(staged));
        manager.discardPreparedSelection(QUrl::fromLocalFile(staged));
        QVERIFY(!QFileInfo::exists(folder));
        QVERIFY(QFile::exists(source));
    }

    void ordinaryTransfersUseRealSftpInBothDirections()
    {
        const QString sshd = QStandardPaths::findExecutable("sshd");
        const QString keygen = QStandardPaths::findExecutable("ssh-keygen");
        if (sshd.isEmpty() || keygen.isEmpty()) QSKIP("OpenSSH tools are unavailable.");
        QTemporaryDir fixture;
        const QString key = fixture.filePath("key");
        QCOMPARE(QProcess::execute(keygen, {"-q", "-t", "ed25519", "-N", "", "-f", key}), 0);
        QTcpServer portReservation;
        QVERIFY(portReservation.listen(QHostAddress::LocalHost, 0));
        const auto port = portReservation.serverPort();
        portReservation.close();
        const QString config = fixture.filePath("sshd_config");
        QFile configuration(config);
        QVERIFY(configuration.open(QIODevice::WriteOnly));
        configuration.write((QString("ListenAddress 127.0.0.1\nPort %1\nHostKey %2\nAuthorizedKeysFile %2.pub\n"
            "PidFile %3\nStrictModes no\nPasswordAuthentication no\nKbdInteractiveAuthentication no\nUsePAM no\n"
            "Subsystem sftp internal-sftp\n").arg(port).arg(key, fixture.filePath("pid"))).toUtf8());
        configuration.close();
        QProcess server;
        server.setProcessChannelMode(QProcess::MergedChannels);
        server.start(sshd, {"-D", "-e", "-f", config});
        QVERIFY(server.waitForStarted());
        auto stopServer = qScopeGuard([&] { server.terminate(); if (!server.waitForFinished(2000)) { server.kill(); server.waitForFinished(); } });
        QByteArray serverLog;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            serverLog += server.readAll();
            return serverLog.contains("Server listening") || server.state() == QProcess::NotRunning;
        })(), 5000);
        QVERIFY2(server.state() != QProcess::NotRunning, serverLog.constData());
        libssh2_init(0);
        SftpConnection local, remote;
        local.local = true;
        remote.socket.connectToHost(QHostAddress::LocalHost, port);
        QVERIFY(remote.socket.waitForConnected(5000));
        remote.session = libssh2_session_init();
        QVERIFY(remote.session);
        libssh2_session_set_timeout(remote.session, 5000);
        QCOMPARE(libssh2_session_handshake(remote.session, remote.socket.socketDescriptor()), 0);
        const auto account = pilonLocalDevice().value("username").toString().toUtf8();
        const auto privateKey = QFile::encodeName(key);
        QCOMPARE(libssh2_userauth_publickey_fromfile(remote.session, account.constData(), nullptr, privateKey.constData(), ""), 0);
        remote.sftp = libssh2_sftp_init(remote.session);
        QVERIFY(remote.sftp);
        QCOMPARE(remoteCommand(remote, "printf pilon-command"), QByteArray("pilon-command"));
        const QString source = fixture.filePath("source"), uploaded = fixture.filePath("uploaded"), downloaded = fixture.filePath("downloaded");
        QVERIFY(QDir().mkpath(source + "/nested"));
        QVERIFY(writeVersion(source + "/file", "source bytes", 3500));
        QVERIFY(writeVersion(source + "/nested/child", "child bytes", 4500));
        QVERIFY(QFile::link("missing-target", source + "/alias"));
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        auto result = copyRemotePath(local, remote, source, uploaded, true, cancel);
        QVERIFY2(result.value("status") == "ok", qPrintable(result.value("error").toString()));
        QCOMPARE(contents(uploaded + "/file"), QByteArray("source bytes"));
        QCOMPARE(contents(uploaded + "/nested/child"), QByteArray("child bytes"));
        QCOMPARE(QFileInfo(uploaded + "/alias").readSymLink(), QString("missing-target"));
        result = copyRemotePath(remote, local, uploaded, downloaded, true, cancel);
        QVERIFY2(result.value("status") == "ok", qPrintable(result.value("error").toString()));
        QCOMPARE(contents(downloaded + "/file"), QByteArray("source bytes"));
        QCOMPARE(contents(downloaded + "/nested/child"), QByteArray("child bytes"));
        QCOMPARE(QFileInfo(downloaded + "/alias").readSymLink(), QString("missing-target"));
    }

    void reorderedBookmarksKeepNamesAccountsAndNewItems()
    {
        QSettings settings;
        const auto previous = settings.value(bookmarksKey);
        auto restore = qScopeGuard([&] { settings.setValue(bookmarksKey, previous); });
        settings.remove(bookmarksKey);
        RemoteFileManager manager;
        const auto a = localEndpoint("/one");
        const auto b = localEndpoint("/two", true);
        const auto c = localEndpoint("/three");
        QVERIFY(manager.addBookmarks({a, b, c}, ""));
        manager.nameBookmark(manager.bookmarks()[0].toMap(), "My file");
        auto staleA = manager.bookmarks()[0].toMap();
        staleA.insert("name", "Old name");
        manager.reorderBookmarks({manager.bookmarks()[1], staleA, staleA});
        QCOMPARE(manager.bookmarks().size(), 3);
        QCOMPARE(manager.bookmarks()[0].toMap().value("path").toString(), QString("/two"));
        QCOMPARE(manager.bookmarks()[1].toMap().value("name").toString(), QString("My file"));
        QCOMPARE(manager.bookmarks()[2].toMap().value("path").toString(), QString("/three"));
        RemoteFileManager reopened;
        QCOMPARE(reopened.bookmarks(), manager.bookmarks());
        const auto entry = manager.bookmarks()[1].toMap();
        manager.removeBookmark(entry.value("deviceId").toString(), entry.value("username").toString(), entry.value("path").toString());
        QCOMPARE(manager.bookmarks().size(), 2);
    }

    void localFileDragsOfferUrlsAndPreserveInternalEntries()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("report #1 résumé.pdf");
        QVERIFY(touch(path));
        const QVariantMap entry{{"deviceId", "local"}, {"path", path}, {"name", QFileInfo(path).fileName()}};
        RemoteFileManager manager;
        const auto icon = QUrl::fromLocalFile(QFINDTESTDATA("../../../src/assets/icons/document-text.svg"));
        manager.startDrag({entry}, icon, Qt::white, Qt::white, QPointF(53, 47));
        auto *drag = manager.findChild<QDrag *>();
        QVERIFY(drag);
        QCOMPARE(drag->hotSpot(), QPoint(87, 57));
        QCOMPARE(drag->mimeData()->urls(), QList<QUrl>{QUrl::fromLocalFile(path)});
        QVERIFY(drag->mimeData()->data("text/uri-list").contains("%23"));
        QCOMPARE(QJsonDocument::fromJson(drag->mimeData()->data("application/x-pilon-remote-entries")).toVariant().toList(),
                 QVariantList{entry});
        QVERIFY(drag->supportedActions().testFlag(Qt::CopyAction));
        QVERIFY(drag->supportedActions().testFlag(Qt::MoveAction));
        QVERIFY(QFileInfo::exists(path)); // External Copy cannot remove the source.
        delete drag;
        auto remote = entry;
        remote.insert("deviceId", "remote");
        manager.startDrag({entry, remote}, icon, Qt::white, Qt::white, QPointF(22, 22));
        drag = manager.findChild<QDrag *>();
        QVERIFY(drag);
        QVERIFY(!drag->mimeData()->hasUrls()); // A mixed selection is never a partial upload.
        QVERIFY(drag->mimeData()->hasFormat("application/x-pilon-remote-entries"));
        delete drag;
    }

    void dragPreviewUsesProvidedFileIconAndName()
    {
        RemoteFileManager manager;
        QVariantMap entry{{"deviceId", "remote"}, {"path", "/image.png"}, {"name", "image.png"}};
        const auto preview = [&](const char *icon) {
            manager.startDrag({entry}, QUrl::fromLocalFile(QString::fromUtf8(icon)), QColor("#aabbcc"), Qt::white, QPointF(22, 22));
            auto *drag = manager.findChild<QDrag *>();
            const QImage image = drag->pixmap().toImage();
            delete drag;
            return image;
        };
        const auto photoIcon = QFINDTESTDATA("../../../src/assets/icons/photo.svg").toUtf8();
        const auto folderIcon = QFINDTESTDATA("../../../src/assets/icons/folder.svg").toUtf8();
        const auto photo = preview(photoIcon.constData());
        QCOMPARE(photo.deviceIndependentSize(), QSizeF(112, 108));
        const auto folder = preview(folderIcon.constData());
        QVERIFY(photo.copy(34, 10, 44, 44) != folder.copy(34, 10, 44, 44));
        QCOMPARE(photo.copy(10, 60, 92, 38), folder.copy(10, 60, 92, 38));
        entry.insert("name", "A long image filename that wraps onto two lines.png");
        const auto renamed = preview(photoIcon.constData());
        QVERIFY(photo.copy(10, 60, 92, 38) != renamed.copy(10, 60, 92, 38));
        QVERIFY(photo.save("/tmp/pilon-file-drag-preview.png"));
        QVERIFY(renamed.save("/tmp/pilon-file-drag-preview-long.png"));
    }

    void bookmarksPersistFilesFoldersAndAccountsWithoutDuplicates()
    {
        QSettings settings;
        const auto previous = settings.value(bookmarksKey);
        const bool existed = settings.contains(bookmarksKey);
        auto restore = qScopeGuard([&] {
            if (existed) settings.setValue(bookmarksKey, previous);
            else settings.remove(bookmarksKey);
        });
        settings.remove(bookmarksKey);
        QTemporaryDir source;
        QVERIFY(touch(source.filePath("photo.png")));
        QVERIFY(QDir().mkdir(source.filePath("folder")));
        RemoteFileManager manager;
        manager.browse(manager.localDevice(), "", source.path());
        QTRY_VERIFY(!manager.busy());
        QSignalSpy changed(&manager, &RemoteFileManager::bookmarksChanged);
        const auto entries = manager.entries();
        QVERIFY(manager.addBookmarks(entries, ""));
        QCOMPARE(manager.bookmarks().size(), 2);
        QCOMPARE(changed.size(), 1);
        QVERIFY(manager.addBookmarks(entries, ""));
        QCOMPARE(manager.bookmarks().size(), 2);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(manager.entries(), entries);
        QVERIFY(QFileInfo::exists(source.filePath("photo.png")));
        QVariantMap remote{{"deviceId", "remote"}, {"deviceName", "Server"}, {"ip", "100.1.2.3"},
            {"path", "/reports/data.pdf"}, {"name", "data.pdf"}, {"directory", false}, {"username", "alice"}};
        QVERIFY(manager.addBookmarks({remote}, "bob"));
        QCOMPARE(manager.bookmarks().last().toMap().value("username").toString(), QString("alice"));
        QCOMPARE(manager.bookmarks().last().toMap().value("directory").toBool(), false);
        manager.nameBookmark(manager.bookmarks().last().toMap(), "Report");
        QVERIFY(manager.addBookmarks({remote}, "bob"));
        QCOMPARE(manager.bookmarks().last().toMap().value("name").toString(), QString("Report"));
        remote.insert("username", "bob");
        QVERIFY(manager.addBookmarks({remote}, "bob"));
        QCOMPARE(manager.bookmarks().size(), 4);
        RemoteFileManager reopened;
        QCOMPARE(reopened.bookmarks(), manager.bookmarks());
        const auto beforeInvalid = reopened.bookmarks();
        QVERIFY(!reopened.addBookmarks({QVariantMap{{"deviceId", "local"}, {"path", "relative"}}}, ""));
        QCOMPARE(reopened.bookmarks(), beforeInvalid);
        QVERIFY(reopened.addBookmarkUrls({QUrl::fromLocalFile(source.filePath("photo.png")),
                                           QUrl::fromLocalFile(source.filePath("folder"))}));
        QCOMPARE(reopened.bookmarks(), beforeInvalid);
        QVERIFY(QFileInfo::exists(source.filePath("folder")));
    }

    void systemClipboardCopiesAcrossManagersAndExportsLocalUrls()
    {
        QTemporaryDir source;
        QTemporaryDir destination;
        const QString path = source.filePath("report #1 résumé.txt");
        QVERIFY(touch(path));
        RemoteFileManager first;
        RemoteFileManager second;
        QSignalSpy clipboardChanged(&second, &RemoteFileManager::clipboardChanged);
        first.browse(first.localDevice(), "", source.path());
        second.browse(second.localDevice(), "", destination.path());
        QTRY_VERIFY(!first.busy() && !second.busy());
        first.copyToClipboard(first.entries(), false);
        QVERIFY(second.clipboardAvailable());
        QVERIFY(!clipboardChanged.isEmpty());
        QCOMPARE(QGuiApplication::clipboard()->mimeData()->urls(), QList<QUrl>{QUrl::fromLocalFile(path)});
        QVERIFY(QGuiApplication::clipboard()->mimeData()->data("x-special/gnome-copied-files").startsWith("copy\nfile:"));
        second.pasteFromClipboard(second.localDevice(), "", destination.path());
        QTRY_VERIFY(!second.busy());
        QVERIFY2(second.errorMessage().isEmpty(), qPrintable(second.errorMessage()));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(QFileInfo::exists(destination.filePath(QFileInfo(path).fileName())));
        QVERIFY(second.canUndo());
        second.undo();
        QTRY_VERIFY(!second.busy());
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(destination.filePath(QFileInfo(path).fileName())));
        QGuiApplication::clipboard()->clear();
    }

    void cutClipboardMovesAndFailurePreservesClipboardAndBothFiles()
    {
        QTemporaryDir source;
        QTemporaryDir destination;
        QVERIFY(touch(source.filePath("file.txt")));
        QVERIFY(touch(destination.filePath("file.txt")));
        RemoteFileManager first;
        RemoteFileManager second;
        first.browse(first.localDevice(), "", source.path());
        second.browse(second.localDevice(), "", destination.path());
        QTRY_VERIFY(!first.busy() && !second.busy());
        first.copyToClipboard(first.entries(), true);
        const auto signature = clipboardSignature(QGuiApplication::clipboard()->mimeData());
        second.pasteFromClipboard(second.localDevice(), "", destination.path());
        QTRY_VERIFY(!second.busy());
        QVERIFY(!second.errorMessage().isEmpty());
        QCOMPARE(clipboardSignature(QGuiApplication::clipboard()->mimeData()), signature);
        QVERIFY(QFileInfo::exists(source.filePath("file.txt")));
        QVERIFY(QFileInfo::exists(destination.filePath("file.txt")));
        QVERIFY(QFile::remove(destination.filePath("file.txt")));
        second.pasteFromClipboard(second.localDevice(), "", destination.path());
        QTRY_VERIFY(!second.busy());
        QVERIFY2(second.errorMessage().isEmpty(), qPrintable(second.errorMessage()));
        QVERIFY(!QFileInfo::exists(source.filePath("file.txt")));
        QVERIFY(QFileInfo::exists(destination.filePath("file.txt")));
        QVERIFY(!first.clipboardAvailable());
        second.undo();
        QTRY_VERIFY(!second.busy());
        QVERIFY(QFileInfo::exists(source.filePath("file.txt")));
        QVERIFY(!QFileInfo::exists(destination.filePath("file.txt")));
    }

    void externalClipboardAndDropsIncludeDirectories()
    {
        QTemporaryDir source;
        QTemporaryDir destination;
        QVERIFY(QDir().mkpath(source.filePath("folder")));
        QVERIFY(touch(source.filePath("folder/data.txt")));
        const QUrl folder = QUrl::fromLocalFile(source.filePath("folder"));
        auto *mime = new QMimeData;
        mime->setUrls({folder});
        QGuiApplication::clipboard()->setMimeData(mime);
        RemoteFileManager manager;
        manager.pasteFromClipboard(manager.localDevice(), "", destination.path());
        QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.errorMessage().isEmpty(), qPrintable(manager.errorMessage()));
        QVERIFY(QFileInfo::exists(source.filePath("folder/data.txt")));
        QVERIFY(QFileInfo::exists(destination.filePath("folder/data.txt")));
        manager.undo();
        QTRY_VERIFY(!manager.busy());
        manager.pasteLocalUrls({folder}, manager.localDevice(), "", destination.path(), true);
        QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.errorMessage().isEmpty(), qPrintable(manager.errorMessage()));
        QVERIFY(!QFileInfo::exists(source.filePath("folder")));
        QVERIFY(QFileInfo::exists(destination.filePath("folder/data.txt")));
        QGuiApplication::clipboard()->clear();
    }

    void remoteClipboardEntriesNeverBecomeLocalUrls()
    {
        RemoteFileManager manager;
        const QVariantList entries{QVariantMap{{"deviceId", "server"}, {"deviceName", "Server"},
            {"path", "/reports/data.txt"}, {"name", "data.txt"}, {"username", "server-account"}}};
        manager.copyToClipboard(entries, false);
        const auto *mime = QGuiApplication::clipboard()->mimeData();
        QVERIFY(!mime->hasUrls());
        QCOMPARE(clipboardFileEntries(mime), entries);
        QCOMPARE(deviceFromEntry(entries.first().toMap()).value("username").toString(), QString("server-account"));
        QGuiApplication::clipboard()->clear();
    }

    void localDirectoryMovesAcrossFilesystemsAndUndo()
    {
        QTemporaryDir source;
        QTemporaryDir destination("/dev/shm/pilon-files-test-XXXXXX");
        if (!destination.isValid()) QSKIP("A writable /dev/shm is required for this check.");
        QVERIFY(QDir().mkpath(source.filePath("folder/nested")));
        QVERIFY(touch(source.filePath("folder/nested/data.txt")));
        RemoteFileManager manager;
        manager.browse(manager.localDevice(), "", source.path());
        QTRY_VERIFY(!manager.busy());
        manager.pasteEntries(manager.entries(), manager.localDevice(), "", destination.path(), true);
        QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.errorMessage().isEmpty(), qPrintable(manager.errorMessage()));
        QVERIFY(!QFileInfo::exists(source.filePath("folder")));
        QVERIFY(QFileInfo::exists(destination.filePath("folder/nested/data.txt")));
        manager.undo();
        QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.errorMessage().isEmpty(), qPrintable(manager.errorMessage()));
        QVERIFY(QFileInfo::exists(source.filePath("folder/nested/data.txt")));
        QVERIFY(!QFileInfo::exists(destination.filePath("folder")));
    }

    void closingSearchDismissesAuthenticationWithoutStartingAnotherDevice()
    {
        RemoteFileManager manager;
        manager.m_searchMode = true;
        manager.m_pendingRequest = {{"kind", "search"}, {"query", "report"},
            {"devices", QVariantList{device("locked"), device("other")}}};
        manager.m_promptDeviceId = "locked";
        manager.setPrompt("credential", "Password");
        manager.cancel();
        QVERIFY(manager.promptKind().isEmpty());
        QVERIFY(manager.m_pendingRequest.isEmpty());
        QVERIFY(!manager.searching());
    }

    void completedMutationsAlreadyHaveFreshEntries()
    {
        QTemporaryDir directory;
        const QString first = directory.filePath("first.txt");
        QVERIFY(touch(first));
        QVERIFY(QDir().mkdir(directory.filePath("destination")));
        RemoteFileManager manager;
        manager.browse(manager.localDevice(), "", directory.path());
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
        QVariantMap entry;
        for (const auto &value : manager.entries())
            if (value.toMap().value("path") == first) entry = value.toMap();
        QVERIFY(!entry.isEmpty());
        QList<QVariantList> completed;
        connect(&manager, &RemoteFileManager::busyChanged, this, [&] {
            if (!manager.busy()) completed.append(manager.entries());
        });
        const auto hasPath = [](const QVariantList &entries, const QString &path) {
            return std::any_of(entries.cbegin(), entries.cend(), [&](const QVariant &entry) {
                return entry.toMap().value("path") == path;
            });
        };
        manager.moveEntry(entry, "", "renamed.txt");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
        QVERIFY(!completed.isEmpty());
        QVERIFY(!hasPath(completed.first(), first));
        const QString renamed = directory.filePath("renamed.txt");
        QVERIFY(hasPath(completed.first(), renamed));
        for (const auto &value : manager.entries())
            if (value.toMap().value("path") == renamed) entry = value.toMap();
        completed.clear();
        manager.pasteEntries({entry}, manager.localDevice(), "", directory.filePath("destination"), true);
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
        QCOMPARE(manager.currentPath(), directory.path());
        QVERIFY(!hasPath(completed.first(), renamed));
        QVERIFY(QFileInfo::exists(directory.filePath("destination/renamed.txt")));
        manager.undo();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
        QVERIFY(hasPath(manager.entries(), renamed));
        auto missing = entry;
        missing.insert("path", directory.filePath("already-gone.txt"));
        completed.clear();
        manager.deleteEntries({entry, missing}, "");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
        QVERIFY(!hasPath(completed.first(), renamed)); // First delete succeeded before the second failed.
        QVERIFY(!manager.errorMessage().isEmpty());
        QCOMPARE(manager.currentPath(), directory.path());
    }
    void confirmedDeletionRunsAfterAnInFlightFolderRefresh()
    {
        QTemporaryDir folder;
        const auto path = folder.filePath("disposable.txt");
        QVERIFY(touch(path));
        RemoteFileManager manager;
        manager.setAutomaticRefreshEnabled(false);
        manager.browse(manager.localDevice(), {}, folder.path());
        QTRY_VERIFY(!manager.busy());
        const auto selected = manager.entries();
        QCOMPARE(selected.size(), 1);
        QPromise<QVariantMap> pendingRefresh;
        pendingRefresh.start();
        manager.m_pendingRequest = manager.m_listingRequest;
        manager.m_pendingRequest["refresh"] = true;
        manager.setBusy(true);
        manager.m_operationWatcher.setFuture(pendingRefresh.future());
        manager.deleteEntries(selected, {});
        pendingRefresh.addResult({{"status", "ok"}, {"device", manager.localDevice()},
            {"path", folder.path()}, {"entries", selected}});
        pendingRefresh.finish();
        QTRY_VERIFY(!manager.busy());
        QVERIFY2(!QFileInfo::exists(path), "Confirming Delete during a folder refresh must reach the filesystem");
    }

    void externalLocalEditsAndAtomicReplacementRefreshEntries()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("notes.txt");
        QVERIFY(touch(path));
        RemoteFileManager manager;
        manager.browse(manager.localDevice(), "", directory.path());
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
        QSignalSpy changed(&manager, &RemoteFileManager::entriesChanged);
        QSignalSpy location(&manager, &RemoteFileManager::locationChanged);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Edited by an external application.");
        file.close();
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 5000);
        QCOMPARE(manager.entries().first().toMap().value("size").toLongLong(), file.size());
        QCOMPARE(location.size(), 0); // Refreshing is not navigation.
        QSaveFile replacement(path);
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        replacement.write("Replaced atomically.");
        QVERIFY(replacement.commit());
        QTRY_COMPARE_WITH_TIMEOUT(manager.entries().first().toMap().value("size").toLongLong(), qint64(20), 5000);
        QVERIFY(QFile::rename(path, directory.filePath("renamed.txt")));
        QTRY_COMPARE_WITH_TIMEOUT(manager.entries().first().toMap().value("name").toString(), QString("renamed.txt"), 5000);
        QVERIFY(QFile::remove(directory.filePath("renamed.txt")));
        QTRY_VERIFY_WITH_TIMEOUT(manager.entries().isEmpty(), 5000);
    }

    void localEditorReceivesOriginalFile()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("notes with spaces.txt");
        QVERIFY(touch(path));
        QFile editor(directory.filePath("editor"));
        QVERIFY(editor.open(QIODevice::WriteOnly));
        editor.write("#!/bin/sh\nprintf 'saved in the original file' > \"$1\"\n");
        editor.close();
        QVERIFY(editor.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        RemoteFileManager manager;
        manager.m_fileOpenerExecutable = editor.fileName();
        manager.viewFile({{"path", path}, {"deviceId", "local"}}, "");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 5000);
        QCOMPARE(manager.previewKind(), QString("text"));
        QCOMPARE(manager.previewUrl().toLocalFile(), path);
        QCOMPARE(QFileInfo(path).size(), 0);
        manager.openPreviewExternally();
        QTRY_COMPARE_WITH_TIMEOUT(QFileInfo(path).size(), qint64(26), 5000);
        QVERIFY(manager.errorMessage().isEmpty());
    }

    void searchesBeyondOldEntryDepthAndResultLimits()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir().mkpath(directory.filePath("bulk")));
        for (int index = 0; index < 25'010; ++index)
            QVERIFY(touch(directory.filePath("bulk/file-" + QString::number(index))));
        QVERIFY(touch(directory.filePath("website-root")));
        QVERIFY(QDir().mkpath(directory.filePath("matches")));
        for (int index = 0; index < 1'010; ++index)
            QVERIFY(touch(directory.filePath("matches/website-" + QString::number(index))));
        QString deep = directory.path();
        for (int index = 0; index < 40; ++index) deep += "/deep";
        QVERIFY(QDir().mkpath(deep));
        QVERIFY(touch(deep + "/WebSite-deep"));
        QVERIFY(QFile::link(directory.path(), deep + "/loop"));
        QTemporaryDir outside;
        QVERIFY(touch(outside.filePath("website-outside")));
        QVERIFY(QFile::link(outside.path(), directory.filePath("linked-folder")));

        SftpConnection connection;
        connection.local = true;
        int streamed = 0;
        const auto result = searchDirectoryTree(connection, pilonLocalDevice(), directory.path(), "website",
            std::make_shared<std::atomic_bool>(false), [&](const auto &) { ++streamed; });
        QCOMPARE(result.value("status").toString(), QString("ok"));
        QVERIFY2(result.value("error").toString().isEmpty(), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("entries").toList().size(), 1'012);
        QCOMPARE(streamed, 1'012);
        bool foundDeep = false;
        for (const auto &value : result.value("entries").toList()) {
            const auto entry = value.toMap();
            foundDeep |= entry.value("path") == deep + "/WebSite-deep";
            QVERIFY(entry.value("name") != "website-outside");
        }
        QVERIFY(foundDeep);
    }

    void cancellationKeepsStreamedMatches()
    {
        QTemporaryDir directory;
        for (int index = 0; index < 100; ++index)
            QVERIFY(touch(directory.filePath("website-" + QString::number(index))));
        SftpConnection connection;
        connection.local = true;
        const auto cancellation = std::make_shared<std::atomic_bool>(false);
        int streamed = 0;
        const auto result = searchDirectoryTree(connection, pilonLocalDevice(), directory.path(), "website",
            cancellation, [&](const auto &) { if (++streamed == 7) cancellation->store(true); });
        QCOMPARE(result.value("status").toString(), QString("cancelled"));
        QCOMPARE(result.value("entries").toList().size(), 7);
        QCOMPARE(streamed, 7);
    }

    void unreadableDirectoryReportsLocation()
    {
        QTemporaryDir directory;
        const QString missing = directory.filePath("missing");
        SftpConnection connection;
        connection.local = true;
        const auto result = searchDirectoryTree(connection, device("example-device"), missing, "website",
            std::make_shared<std::atomic_bool>(false), {});
        const QString error = result.value("error").toString();
        QVERIFY(error.contains("example-device"));
        QVERIFY(error.contains(missing));
        QVERIFY(error.contains("could not be searched"));
    }

    void everyDeviceIsAttemptedAfterErrors()
    {
        auto first = device("bad-username");
        first.insert("username", "invalid/name");
        const auto result = executeRequest({{"kind", "search"}, {"query", "website"},
            {"devices", QVariantList{first, device("unavailable-device")}}},
            std::make_shared<std::atomic_bool>(false));
        const QString error = result.value("error").toString();
        QVERIFY2(error.contains("bad-username"), qPrintable(error));
        QVERIFY2(error.contains("unavailable-device"), qPrintable(error));
        QVERIFY(!error.contains("limited"));
    }

    void authenticationRetryPreservesOtherDevices()
    {
        RemoteFileManager manager;
        const QVariantList found{QVariantMap{{"deviceId", "server"}, {"path", "/website"}}};
        manager.m_pendingRequest = {{"kind", "search"}, {"query", "website"},
            {"devices", QVariantList{device("server"), device("locked-device")}}};
        manager.m_searchMode = true;
        manager.setSearching(true);
        finishSearch(manager, {{"status", "credential"}, {"searchMode", true}, {"entries", found},
            {"deviceId", "locked-device"}, {"prompt", "Enter the password for locked-device."},
            {"retryDevices", QVariantList{device("locked-device")}}});
        QTRY_VERIFY(!manager.searching());
        QCOMPARE(manager.entries(), found);
        QCOMPARE(manager.promptKind(), QString("credential"));
        QCOMPARE(manager.m_pendingRequest.value("devices").toList().size(), 1);
        manager.submitCredential("fixture-password");
        QCOMPARE(manager.entries(), found);
        QVERIFY(manager.searching());
        QTRY_VERIFY(!manager.searching());
        QCOMPARE(manager.entries(), found);
        QVERIFY(manager.errorMessage().contains("locked-device"));
        QVERIFY(manager.promptKind().isEmpty());
    }

    void dismissingAuthenticationContinuesRemainingDevices()
    {
        RemoteFileManager manager;
        manager.m_entries = {QVariantMap{{"deviceId", "server"}, {"path", "/website"}}};
        const auto found = manager.entries();
        manager.m_pendingRequest = {{"kind", "search"}, {"query", "website"}, {"retainSearchResults", true},
            {"devices", QVariantList{device("first-locked"), device("second-locked")}}};
        manager.m_searchMode = true;
        manager.m_promptDeviceId = "first-locked";
        manager.setPrompt("hostKey", "Trust first-locked?");
        manager.cancelPrompt();
        QTRY_VERIFY(!manager.searching());
        QCOMPARE(manager.entries(), found);
        QVERIFY(manager.errorMessage().contains("first-locked"));
        QVERIFY(manager.errorMessage().contains("authentication was cancelled"));
        QVERIFY(manager.errorMessage().contains("second-locked"));
    }

    void finalResultsAreCommittedBeforeCompletion()
    {
        RemoteFileManager manager;
        manager.m_pendingRequest = {{"kind", "search"}, {"query", "website"}};
        manager.m_searchMode = true;
        manager.setSearching(true);
        bool completeWithResults = false;
        connect(&manager, &RemoteFileManager::searchingChanged, this, [&] {
            if (!manager.searching()) completeWithResults = manager.entries().size() == 1;
        });
        finishSearch(manager, {{"status", "cancelled"}, {"searchMode", true},
            {"entries", QVariantList{QVariantMap{{"path", "/website"}}}}});
        QTRY_VERIFY(!manager.searching());
        QVERIFY(completeWithResults);
    }

    void localPickerAcceptsFileUrlsIncludingEncodedNames() {
        QTemporaryDir root;
        const auto path = root.filePath("folder # one"); QVERIFY(QDir().mkpath(path));
        QVERIFY(touch(path + "/existing.pdf"));
        RemoteFileManager manager;
        manager.browse(manager.localDevice(), "", QUrl::fromLocalFile(path).toString());
        QTRY_COMPARE(manager.currentPath(), path); QTRY_VERIFY(!manager.busy());
        QVERIFY2(manager.errorMessage().isEmpty(), qPrintable(manager.errorMessage()));
        QCOMPARE(manager.entries().size(), 1);
        manager.browse(manager.localDevice(), "", QUrl::fromLocalFile(QDir::homePath()).toString());
        QTRY_COMPARE(manager.currentPath(), QDir::homePath()); QTRY_VERIFY(!manager.busy());
        QVERIFY(manager.errorMessage().isEmpty());
    }

    void navigationDiscardsOldSearchCallbacks()
    {
        QTemporaryDir directory;
        QVERIFY(touch(directory.filePath("new-location")));
        RemoteFileManager manager;
        manager.m_searchMode = true;
        manager.m_requestGeneration = 3;
        manager.m_pendingRequest = {{"kind", "search"}, {"query", "website"}};
        manager.m_cancelRequested = std::make_shared<std::atomic_bool>(false);
        manager.setSearching(true);
        QPromise<QVariantMap> operation;
        operation.start();
        manager.m_operationWatcher.setFuture(operation.future());
        manager.browse(pilonLocalDevice(), "", directory.path());
        QVERIFY(manager.m_cancelRequested->load());
        manager.appendSearchEntry(3, {{"path", "/stale"}});
        QVERIFY(manager.entries().isEmpty());
        operation.addResult({{"status", "cancelled"}});
        operation.finish();
        QTRY_COMPARE(manager.currentPath(), directory.path());
        QTRY_VERIFY(!manager.busy());
        manager.appendSearchEntry(3, {{"path", "/stale"}});
        QCOMPARE(manager.entries().size(), 1);
        QCOMPARE(manager.entries().first().toMap().value("name").toString(), QString("new-location"));
    }

    void liveTailscaleAuthentication()
    {
        const auto host = qEnvironmentVariable("PILON_TEST_SSH_HOST");
        if (host.isEmpty()) QSKIP("Set PILON_TEST_SSH_HOST, PILON_TEST_SSH_DEVICE_ID and PILON_TEST_FILE_SETTINGS to test Tailscale SSH.");
        const auto id = qEnvironmentVariable("PILON_TEST_SSH_DEVICE_ID");
        QSettings configured(qEnvironmentVariable("PILON_TEST_FILE_SETTINGS"), QSettings::IniFormat);
        const auto accounts = QJsonDocument::fromJson(configured.value("Pilon/deviceUsernamesJson").toString().toUtf8()).object();
        const auto account = accounts.value(id).toString();
        QVERIFY(!account.isEmpty());
        const auto key = QByteArray::fromBase64(configured.value(trustedHostKeySetting(id)).toByteArray());
        QVERIFY(!key.isEmpty());
        const QVariantMap target{{"id", id}, {"name", "Tailscale SSH fixture"}, {"ip", host}, {"online", true}};
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        SftpConnection accepted;
        const auto result = authenticate(accepted, target, account, {}, false, key, cancel);
        QVERIFY2(result.isEmpty(), qPrintable(result.value("error").toString()));
        QVERIFY(libssh2_userauth_authenticated(accepted.session));
        QVERIFY(accepted.sftp);
        SftpConnection rejected;
        const auto denial = authenticate(rejected, target, "pilon-user-does-not-exist", {}, false, key, cancel);
        QCOMPARE(denial.value("status"), QString("error"));
        QVERIFY2(denial.value("error").toString().contains("tailscale", Qt::CaseInsensitive),
                 qPrintable(denial.value("error").toString()));
    }

    void liveAllDevicesSearch()
    {
        const QString settingsPath = qEnvironmentVariable("PILON_TEST_FILE_SETTINGS");
        if (settingsPath.isEmpty()) QSKIP("Set PILON_TEST_FILE_SETTINGS to test read-only search on configured devices.");
        const QString serverName = qEnvironmentVariable("PILON_TEST_REMOTE_SERVER");
        if (serverName.isEmpty()) QSKIP("Set PILON_TEST_REMOTE_SERVER to select the configured search device.");
        QSettings configured(settingsPath, QSettings::IniFormat);
        QProcess tailscale;
        tailscale.start("tailscale", {"status", "--json"});
        QVERIFY(tailscale.waitForFinished(10000));
        QCOMPARE(tailscale.exitCode(), 0);
        const auto status = QJsonDocument::fromJson(tailscale.readAllStandardOutput()).object();
        const auto usernames = QJsonDocument::fromJson(configured.value("Pilon/deviceUsernamesJson").toString().toUtf8()).object();
        const auto included = configured.value("tailscale/includedDevices").toStringList();
        const QString username = configured.value("Pilon/lastSshUsername").toString();
        QVariantList selected{pilonLocalDevice()};
        QVariantMap server;
        QJsonArray peers{status.value("Self")};
        for (const auto &value : status.value("Peer").toObject()) peers.append(value);
        QSettings fixture;
        QStringList temporaryKeys;
        auto restoreKeys = qScopeGuard([&] { for (const auto &key : temporaryKeys) fixture.remove(key); });
        for (const auto &value : peers) {
            const auto peer = value.toObject();
            const QString id = peer.value("ID").toString();
            if (!peer.value("Online").toBool() || !included.contains(id)) continue;
            QVariantMap target{{"id", id}, {"name", peer.value("HostName").toString()},
                {"ip", peer.value("TailscaleIPs").toArray().first().toString()}, {"online", true},
                {"username", usernames.value(id).toString(username)}};
            selected.append(target);
            if (target.value("name") == serverName) server = target;
            const QString key = trustedHostKeySetting(id);
            QVERIFY(!fixture.contains(key));
            fixture.setValue(key, configured.value(key));
            temporaryKeys.append(key);
        }
        QVERIFY(!server.isEmpty());
        RemoteFileManager manager;
        manager.search({server}, username, "website");
        const bool firstMatch = QTest::qWaitFor([&] { return !manager.entries().isEmpty() || !manager.searching(); }, 120000);
        manager.cancel();
        QVERIFY(QTest::qWaitFor([&] { return !manager.searching(); }, 30000));
        QVERIFY(firstMatch);
        QVERIFY2(manager.promptKind().isEmpty(), qPrintable(manager.promptText()));
        QSet<QString> expected;
        for (const auto &value : manager.entries()) expected.insert(value.toMap().value("path").toString());
        QVERIFY(!expected.isEmpty());
        qInfo() << "Single-device matches:" << expected.size();
        manager.search(selected, username, "website");
        QSet<QString> actual;
        const bool found = QTest::qWaitFor([&] {
            actual.clear();
            for (const auto &value : manager.entries()) {
                const auto entry = value.toMap();
                if (entry.value("deviceId") == server.value("id")) actual.insert(entry.value("path").toString());
            }
            return actual.contains(expected) || !manager.searching();
        }, 120000);
        manager.cancel();
        QVERIFY(QTest::qWaitFor([&] { return !manager.searching(); }, 30000));
        QVERIFY2(found && actual.contains(expected), qPrintable(manager.errorMessage()));
        qInfo() << "All-device search retained" << actual.size() << "server matches across" << selected.size() << "selected devices.";
        qInfo().noquote() << manager.errorMessage();
    }
};

QTEST_MAIN(RemoteFileTests)
#include "remotefile_tests.moc"
