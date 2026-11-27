#include "sshmanager.h"
#include "sshcredentials.h"
#include "../../../src/core/network/networksnapshot.h"
#include <QCryptographicHash>
#include <QFile>
#include <QSettings>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>



class SshTests : public QObject {
    Q_OBJECT
    QTemporaryDir m_fixture;
    QByteArray m_oldPath;
private slots:
    void initTestCase() {
        QVERIFY(m_fixture.isValid());
        QCoreApplication::setOrganizationName("PilonTests");
        QCoreApplication::setApplicationName("SSH");
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, m_fixture.path());
        QFile ssh(m_fixture.filePath("ssh"));
        QVERIFY(QFile::copy(QFINDTESTDATA("../../../addons/ssh/tests/fixtures/ssh.py"), ssh.fileName()));
        QVERIFY(ssh.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(ssh.copy(m_fixture.filePath("scp")));
        QFile secret(m_fixture.filePath("secret-tool"));
        QVERIFY(secret.open(QIODevice::WriteOnly)); secret.write("#!/bin/sh\nexit 1\n"); secret.close();
        QVERIFY(secret.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        m_oldPath = qgetenv("PATH");
        qputenv("PATH", m_fixture.path().toUtf8() + ':' + m_oldPath);
    }
    void cleanupTestCase() { qputenv("PATH", m_oldPath); qunsetenv("PILON_TEST_SSH_MODE"); }
    void deviceAliasUsesOnlyTheDiscoveredPeer_data() {
        QTest::addColumn<QByteArray>("configuration");
        QTest::addColumn<QString>("alias");
        QTest::addColumn<QByteArray>("destination");
        QTest::newRow("same-peer") << QByteArray("hostname 192.0.2.10\n")
            << QString("peer-alias") << QByteArray("peer-alias");
        QTest::newRow("different-peer") << QByteArray("hostname 192.0.2.20\n")
            << QString("peer-alias") << QByteArray("192.0.2.10");
        QTest::newRow("proxy") << QByteArray("hostname 192.0.2.10\nproxyjump bastion\n")
            << QString("peer-alias") << QByteArray("192.0.2.10");
        QTest::newRow("invalid-alias") << QByteArray("hostname 192.0.2.10\n")
            << QString("-F") << QByteArray("192.0.2.10");
    }
    void deviceAliasUsesOnlyTheDiscoveredPeer() {
        QFETCH(QByteArray, configuration);
        QFETCH(QString, alias);
        QFETCH(QByteArray, destination);
        qputenv("PILON_TEST_SSH_MODE", "device-alias");
        qputenv("PILON_TEST_SSH_CONFIG", configuration);
        const QString log = m_fixture.filePath("device-ssh-arguments");
        QFile::remove(log);
        qputenv("PILON_TEST_SSH_ARGUMENTS", log.toUtf8());
        auto restore = qScopeGuard([] {
            qunsetenv("PILON_TEST_SSH_CONFIG");
            qunsetenv("PILON_TEST_SSH_ARGUMENTS");
        });
        SshManager manager;
        QVERIFY(QMetaObject::invokeMethod(&manager, "connectToDevice",
            QString("tester"), QString("192.0.2.10"), alias, QString("/reports")));
        QTRY_VERIFY_WITH_TIMEOUT(manager.authenticated(), 3000);
        QFile file(log);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto arguments = file.readAll().split('\n');
        QVERIFY(arguments.contains("StrictHostKeyChecking=ask"));
        QCOMPARE(arguments.last(3), QList<QByteArray>({"tester", destination,
            "cd -- '/reports' && printf '\\033[3J\\033[2J\\033[H' && exec \"${SHELL:-/bin/sh}\" -l"}));
        QVERIFY(manager.savedConnections().contains(QVariantMap{{"host", "192.0.2.10"},
                                                                 {"username", "tester"}}));
        manager.disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
    }
    void deviceAliasRetryRechecksTheSelectedPeer() {
        qputenv("PILON_TEST_SSH_MODE", "device-alias-retry");
        qputenv("PILON_TEST_SSH_CONFIG", "hostname 192.0.2.20\n");
        const auto log = m_fixture.filePath("device-ssh-retry-arguments");
        qputenv("PILON_TEST_SSH_ARGUMENTS", log.toUtf8());
        auto restore = qScopeGuard([] {
            qunsetenv("PILON_TEST_SSH_CONFIG");
            qunsetenv("PILON_TEST_SSH_ARGUMENTS");
        });
        SshManager manager;
        manager.connectToDevice("tester", "192.0.2.10", "peer-alias", {});
        QTRY_VERIFY_WITH_TIMEOUT(manager.canRetry() && !manager.running(), 3000);
        qputenv("PILON_TEST_SSH_CONFIG", "hostname 192.0.2.10\n");
        manager.retryConnection();
        QTRY_VERIFY_WITH_TIMEOUT(manager.authenticated(), 3000);
        QFile file(log);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll().split('\n').last(), "peer-alias");
        manager.disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
    }
    void x11ForwardingIsOptInAndOnlyForTerminals() {
        qputenv("PILON_TEST_SSH_MODE", "x11-options");
        const QString log = m_fixture.filePath("ssh-arguments");
        qputenv("PILON_TEST_SSH_ARGUMENTS", log.toUtf8());
        const auto oldDisplay = qgetenv("DISPLAY");
        qputenv("DISPLAY", "localhost:73");
        auto restore = qScopeGuard([&] {
            oldDisplay.isNull() ? qunsetenv("DISPLAY") : qputenv("DISPLAY", oldDisplay);
            qunsetenv("PILON_TEST_SSH_ARGUMENTS");
        });
        SshManager manager;
        const auto capture = [&](auto action, const QByteArray &expected) {
            QFile file(log);
            if (file.exists()) QVERIFY(file.remove());
            action();
            QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto arguments = file.readAll().split('\n');
            QVERIFY(arguments.contains(expected));
            QCOMPARE(arguments.contains("-Y"), expected == "-Y");
        };
        const auto terminal = [&] { manager.connectToHost("tester", "host.fixture.invalid"); };
        capture(terminal, "-x");
        QVERIFY(manager.setProperty("trustedX11Forwarding", true));
        capture(terminal, "-Y");
        capture([&] { manager.runCommand("tester", "host.fixture.invalid", "pwd"); }, "-x");
        capture([&] { manager.transferFile("tester", "host.fixture.invalid", log, "/report", true); }, "ForwardX11=no");
        qunsetenv("DISPLAY");
        QVERIFY(!manager.x11DisplayAvailable());
        capture(terminal, "-x");
    }
#ifdef Q_OS_LINUX
    void x11ForwardingReportsAMissingLocalSocket() {
        qputenv("PILON_TEST_SSH_MODE", "x11-options");
        const auto log = m_fixture.filePath("missing-x11-ssh-arguments");
        QFile::remove(log);
        qputenv("PILON_TEST_SSH_ARGUMENTS", log.toUtf8());
        const auto oldDisplay = qgetenv("DISPLAY");
        qputenv("DISPLAY", "unix:4294967294.0");
        const auto restore = qScopeGuard([&] {
            oldDisplay.isNull() ? qunsetenv("DISPLAY") : qputenv("DISPLAY", oldDisplay);
            qunsetenv("PILON_TEST_SSH_ARGUMENTS");
        });
        SshManager manager;
        QVERIFY(manager.setProperty("trustedX11Forwarding", true));
        manager.connectToHost("tester", "host.fixture.invalid");
        QVERIFY(!manager.running());
        QVERIFY(manager.errorDetails().contains("/tmp/.X11-unix/X4294967294"));
        QVERIFY(!QFileInfo::exists(log));
    }
#endif
    void forgettingPasswordsKeepsSecretsOnFailureAndWaitsForPendingStores() {
        const auto tool=m_fixture.filePath("secret-tool");
        const auto write=[&](const QByteArray &script) {QFile file(tool);if(!file.open(QIODevice::WriteOnly))return false;file.write(script);file.close();return file.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);};
        auto restore=qScopeGuard([&]{write("#!/bin/sh\nexit 1\n");});
        QVERIFY(write("#!/bin/sh\nprintf 'locked wallet\\n' >&2\nexit 1\n"));
        const auto sizes = NetworkSnapshot::path("sftp-sizes", {m_fixture.path()});
        NetworkSnapshot::write(sizes, {{"fixture", 1}});
        QVERIFY(QFile::exists(sizes));
        SshCredentials::put("fixture","user","fixture-password");
        SshManager manager;
        QVERIFY(!manager.clearSavedPasswords().isEmpty());
        QVERIFY(manager.errorMessage().isEmpty());
        QVERIFY(!manager.forgetPasswords());
        QVERIFY(!manager.errorMessage().isEmpty());QCOMPARE(SshCredentials::get("fixture","user"),QString("fixture-password"));
        QProcess pending;SshCredentials::trackWrite(&pending);
        QVERIFY(!manager.cleanupBlockedReason().isEmpty());QVERIFY(!manager.forgetPasswords());
        QVERIFY(write("#!/bin/sh\n[ \"$*\" = 'clear application pilon protocol ssh' ]\n"));
        pending.start("/nonexistent/pilon-test-process");QVERIFY(!pending.waitForStarted());
        QVERIFY(manager.cleanupBlockedReason().isEmpty());
        QVERIFY(manager.forgetPasswords());QVERIFY(manager.errorMessage().isEmpty());
        QVERIFY(!QFile::exists(sizes));
        QVERIFY(SshCredentials::get("fixture","user").isEmpty());
        QVERIFY(write("#!/bin/sh\nexit 1\n"));QVERIFY(manager.forgetPasswords());
    }
    void shortFailurePreservesTheActualError() {
        qputenv("PILON_TEST_SSH_MODE", "failure");
        SshManager manager;
        manager.connectToHost("tester", "host.fixture.invalid");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
        QCOMPARE(manager.errorMessage(), "Could not find the SSH host.");
        QVERIFY(manager.errorDetails().contains("Could not resolve hostname unavailable.fixture.invalid"));
        QVERIFY(manager.canRetry());
    }
    void networkFailureSummaries_data() {
        QTest::addColumn<QByteArray>("mode");
        QTest::addColumn<QString>("summary");
        QTest::addColumn<bool>("retry");
        QTest::newRow("refused") << QByteArray("refused") << QString("The device refused the SSH connection.") << true;
        QTest::newRow("timeout") << QByteArray("timeout") << QString("The SSH connection timed out.") << true;
        QTest::newRow("denied") << QByteArray("denied") << QString("Permission denied by SSH.") << false;
    }
    void networkFailureSummaries() {
        QFETCH(QByteArray, mode);
        QFETCH(QString, summary);
        QFETCH(bool, retry);
        qputenv("PILON_TEST_SSH_MODE", mode);
        SshManager manager;
        manager.connectToHost("tester", "host.fixture.invalid");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
        QCOMPARE(manager.errorMessage(), summary);
        QCOMPARE(manager.canRetry(), retry);
        QVERIFY(!manager.errorDetails().isEmpty());
        manager.connectToHost("tester", "invalid host!");
        QCOMPARE(manager.errorMessage(), "Enter a valid IP address or host name.");
        QVERIFY(manager.errorDetails().isEmpty());
        QVERIFY(!manager.canRetry());
    }
    void changedHostDoesNotOfferTrustOrRetry() {
        qputenv("PILON_TEST_SSH_MODE", "changed-key");
        SshManager manager;
        manager.connectToHost("tester", "keys.fixture.invalid");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
        QCOMPARE(manager.errorMessage(), "The SSH host key changed. Verify the new key with the device owner.");
        QVERIFY(manager.errorDetails().contains("REMOTE HOST IDENTIFICATION HAS CHANGED"));
        QCOMPARE(manager.promptKind(), SshManager::NoPrompt);
        QVERIFY(!manager.canRetry());
    }
    void retryAfterRejectedSavedPasswordOffersNewInput() {
        qputenv("PILON_TEST_SSH_MODE", "password");
        const QString host = "retry.fixture.invalid", user = "tester";
        auto restore = qScopeGuard([&] { SshCredentials::put(host, user, {}); });
        SshCredentials::put(host, user, "incorrect-saved-password");
        SshManager manager;
        manager.connectToHost(user, host);
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
        QCOMPARE(manager.errorMessage(), "SSH authentication failed.");
        QVERIFY(!manager.errorDetails().contains("incorrect-saved-password"));
        QVERIFY(manager.canRetry());
        manager.retryConnection();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.promptText().isEmpty(), 3000);
        QCOMPARE(manager.errorMessage(), QString{});
        manager.submitPrompt("test-password");
        QTRY_VERIFY_WITH_TIMEOUT(manager.authenticated(), 3000);
        manager.disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
    }
    void challengeUsesTerminalEchoMode() {
        qputenv("PILON_TEST_SSH_MODE", "challenge");
        SshManager manager;
        manager.connectToHost("tester", "challenge.fixture.invalid");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.promptText().isEmpty(), 3000);
        QCOMPARE(manager.promptKind(), SshManager::ChallengePrompt);
        QVERIFY(!manager.promptSecret());
        QVERIFY(!manager.promptPassword());
        manager.submitPrompt("1");
        QTRY_VERIFY_WITH_TIMEOUT(manager.authenticated(), 3000);
        manager.disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
    }
    void shortSuccessfulLogoutHasNoError() {
        qputenv("PILON_TEST_SSH_MODE", "logout");
        SshManager manager;
        QSignalSpy authenticated(&manager, &SshManager::authenticatedChanged);
        manager.connectToHost("tester", "host.fixture.invalid");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
        QVERIFY(authenticated.size() >= 1);
        QVERIFY(manager.errorMessage().isEmpty());
    }
    void slowTerminalReceivesAllPastedInput() {
        qputenv("PILON_TEST_SSH_MODE", "terminal-input");
        SshManager manager;
        QSignalSpy output(&manager, &SshManager::terminalData);
        manager.connectToHost("tester", "host.fixture.invalid");
        QTRY_VERIFY_WITH_TIMEOUT(manager.authenticated(), 3000);
        const QByteArray text = QByteArray("abcDEF_1234").repeated(256 * 1024 / 11 + 1)
                                    .left(256 * 1024);
        manager.writeTerminalData(text);
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
        QByteArray received;
        for (const auto &data : output)
            received += data.first().toByteArray();
        QCOMPARE(received.trimmed(), QCryptographicHash::hash(text, QCryptographicHash::Sha256).toHex());
    }
    void terminalCanResizeToOneRow() {
        qputenv("PILON_TEST_SSH_MODE", "terminal-size");
        SshManager manager;
        QSignalSpy output(&manager, &SshManager::terminalData);
        manager.connectToHost("tester", "host.fixture.invalid");
        QTRY_VERIFY_WITH_TIMEOUT(manager.authenticated(), 3000);
        manager.resizeTerminal(1, 80);
        manager.writeTerminalData("\n");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
        QByteArray received;
        for (const auto &data : output)
            received += data.first().toByteArray();
        QCOMPARE(received.trimmed(), QByteArray("1 80"));
    }
    void taskOutputReportsWhenItReachesTheLimit() {
        qputenv("PILON_TEST_SSH_MODE", "long-task");
        SshManager manager;
        QSignalSpy finished(&manager, &SshManager::taskFinished);
        manager.runCommand("tester", "host.fixture.invalid", "fixture");
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QCOMPARE(finished.first().at(0).toInt(), 0);
        const QString output = finished.first().at(1).toString();
        QVERIFY(output.startsWith("START\n"));
        QVERIFY(output.size() < 2 * 1024 * 1024);
        QVERIFY(output.endsWith("SSH command output was truncated."));
    }
    void firstHostConfirmationIncludesIdentityAndFingerprint() {
        qputenv("PILON_TEST_SSH_MODE", "host");
        SshManager manager;
        manager.connectToHost("tester", "host.fixture.invalid");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.promptText().isEmpty(), 3000);
        const auto details = manager.property("connectionText").toString();
        QVERIFY(details.contains("host.fixture.invalid (192.0.2.1)"));
        QVERIFY(details.contains("SHA256:fixtureFingerprint"));
        QVERIFY(manager.promptText().startsWith("Are you sure"));
        QCOMPARE(manager.promptKind(), SshManager::HostTrustPrompt);
        QVERIFY(!manager.promptSecret());
        manager.submitPrompt("yes");
        QVERIFY(!manager.promptText().isEmpty());
        manager.trustHost();
        QTRY_VERIFY_WITH_TIMEOUT(manager.authenticated(), 3000);
        QVERIFY(manager.property("connectionText").toString().isEmpty());
        manager.disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
    }
    void browserApprovalInstructionsDoNotRequireAnInputPrompt() {
        qputenv("PILON_TEST_SSH_MODE", "approval");
        SshManager manager;
        manager.connectToHost("tester", "host.fixture.invalid");
        QTRY_VERIFY_WITH_TIMEOUT(manager.property("connectionText").toString().contains("https://login.fixture.invalid/approve"), 3000);
        QVERIFY(manager.promptText().isEmpty());
        QVERIFY(manager.running()); QVERIFY(!manager.authenticated());
        manager.disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
        QVERIFY(manager.property("connectionText").toString().isEmpty());
    }
    void passwordIsNotIncludedInConnectionDetails() {
        qputenv("PILON_TEST_SSH_MODE", "password");
        SshManager manager;
        manager.connectToHost("tester", "password.fixture.invalid");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.promptText().isEmpty(), 3000);
        QVERIFY(manager.promptSecret());
        QStringList displayed;
        connect(&manager, &SshManager::promptChanged, this, [&] { displayed.append(manager.promptText()); });
        connect(&manager, &SshManager::errorMessageChanged, this, [&] { displayed.append(manager.errorMessage()); });
        manager.submitPrompt("test-password");
        QTRY_VERIFY_WITH_TIMEOUT(manager.authenticated(), 3000);
        QVERIFY(!manager.property("connectionText").toString().contains("test-password"));
        QVERIFY(!displayed.join('\n').contains("test-password"));
        manager.disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
    }
    void unavailableKeyringAllowsManualPassword() {
        qputenv("PILON_TEST_SSH_MODE", "password");
        QFile keyring(m_fixture.filePath("secret-tool"));
        const auto writeScript = [&](const QByteArray &script) {
            if (!keyring.open(QIODevice::WriteOnly)) return false;
            const bool written = keyring.write(script) == script.size();
            keyring.close();
            return written;
        };
        const auto restore = qScopeGuard([&] { writeScript("#!/bin/sh\nexit 1\n"); });
        QVERIFY(writeScript("#!/bin/sh\nprintf 'keyring locked\\n' >&2\nexit 1\n"));
        SshManager manager;
        manager.connectToHost("tester", "keyring.fixture.invalid");
        QTRY_COMPARE_WITH_TIMEOUT(manager.promptKind(), SshManager::PasswordPrompt, 3000);
        QCOMPARE(manager.connectionText(), "Could not read saved SSH passwords. Enter your password.");
        manager.submitPrompt("test-password");
        QTRY_VERIFY_WITH_TIMEOUT(manager.authenticated(), 3000);
        QVERIFY(manager.connectionText().isEmpty());
        manager.disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
    }
    void savingPasswordRequiresAuthentication_data() {
        QTest::addColumn<QString>("password");
        QTest::addColumn<bool>("cancel");
        QTest::addColumn<bool>("authenticated");
        QTest::newRow("accepted") << QString("test-password") << false << true;
        QTest::newRow("rejected") << QString("wrong-password") << false << false;
        QTest::newRow("cancelled") << QString("test-password") << true << false;
    }
    void savingPasswordRequiresAuthentication() {
        QFETCH(QString, password);
        QFETCH(bool, cancel);
        QFETCH(bool, authenticated);
        const QString host = "password-save.fixture.invalid", user = "tester";
        qputenv("PILON_TEST_SSH_MODE", "password");
        const auto cleanup = qScopeGuard([&] { SshCredentials::put(host, user, {}); });
        SshManager manager;
        QVERIFY(manager.passwordSavingAvailable());
        manager.connectToHost(user, host);
        QTRY_VERIFY_WITH_TIMEOUT(!manager.promptText().isEmpty(), 3000);
        QVERIFY(manager.promptPassword());

        const QString previous = "previous-password";
        SshCredentials::put(host, user, previous);
        manager.submitPromptAndSave(password);
        QCOMPARE(SshCredentials::get(host, user), previous);
        if (cancel)
            manager.disconnectFromHost();
        if (authenticated) {
            QTRY_VERIFY_WITH_TIMEOUT(manager.authenticated(), 3000);
            manager.disconnectFromHost();
        }
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
        QCOMPARE(SshCredentials::get(host, user), authenticated ? password : previous);
        QTRY_VERIFY(!SshCredentials::hasPendingWrites());
    }
    void successfulTransferSavesPasswordAfterCompletion() {
        const QString host = "transfer-save.fixture.invalid", user = "tester";
        qputenv("PILON_TEST_SSH_MODE", "transfer-password");
        const auto cleanup = qScopeGuard([&] { SshCredentials::put(host, user, {}); });
        SshManager manager;
        QSignalSpy finished(&manager, &SshManager::taskFinished);
        manager.transferFile(user, host, m_fixture.filePath("file"), "/file", true);
        QTRY_VERIFY_WITH_TIMEOUT(manager.promptPassword() && !manager.promptText().isEmpty(), 3000);
        manager.submitPromptAndSave("test-password");
        QVERIFY(SshCredentials::get(host, user).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QCOMPARE(finished[0][0].toInt(), 0);
        QCOMPARE(SshCredentials::get(host, user), QString("test-password"));
        QTRY_VERIFY(!SshCredentials::hasPendingWrites());
    }
    void forgettingPasswordsDiscardsAnotherManagersPendingSave() {
        const QString host = "password-challenge.fixture.invalid", user = "tester";
        qputenv("PILON_TEST_SSH_MODE", "password-challenge");
        const auto cleanup = qScopeGuard([&] { SshCredentials::put(host, user, {}); });
        SshManager connection;
        connection.connectToHost(user, host);
        QTRY_VERIFY_WITH_TIMEOUT(connection.promptPassword() && !connection.promptText().isEmpty(), 3000);
        connection.submitPromptAndSave("test-password");
        QTRY_COMPARE_WITH_TIMEOUT(connection.promptText(), QString("Verification code:"), 3000);
        QCOMPARE(connection.promptKind(), SshManager::SecretPrompt);
        QTRY_VERIFY(!SshCredentials::hasPendingWrites());

        SshManager settings;
        QVERIFY(settings.forgetPasswords());
        connection.submitPrompt("fixture-code");
        QTRY_VERIFY_WITH_TIMEOUT(connection.authenticated(), 3000);
        QVERIFY(SshCredentials::get(host, user).isEmpty());
        connection.disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(!connection.running(), 3000);
    }
};
QTEST_GUILESS_MAIN(SshTests)
#include "ssh_tests.moc"
