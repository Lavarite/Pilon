#include "sshmanager.h"
#include "sshcredentials.h"
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
        QFile secret(m_fixture.filePath("secret-tool"));
        QVERIFY(secret.open(QIODevice::WriteOnly)); secret.write("#!/bin/sh\nexit 1\n"); secret.close();
        QVERIFY(secret.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        m_oldPath = qgetenv("PATH");
        qputenv("PATH", m_fixture.path().toUtf8() + ':' + m_oldPath);
    }
    void cleanupTestCase() { qputenv("PATH", m_oldPath); qunsetenv("PILON_TEST_SSH_MODE"); }
    void forgettingPasswordsKeepsSecretsOnFailureAndWaitsForPendingStores() {
        const auto tool=m_fixture.filePath("secret-tool");
        const auto write=[&](const QByteArray &script) {QFile file(tool);if(!file.open(QIODevice::WriteOnly))return false;file.write(script);file.close();return file.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);};
        auto restore=qScopeGuard([&]{write("#!/bin/sh\nexit 1\n");});
        QVERIFY(write("#!/bin/sh\nprintf 'locked wallet\\n' >&2\nexit 1\n"));
        SshCredentials::put("fixture","user","fixture-password");
        SshManager manager;QVERIFY(!manager.forgetPasswords());
        QVERIFY(!manager.errorMessage().isEmpty());QCOMPARE(SshCredentials::get("fixture","user"),QString("fixture-password"));
        QProcess pending;SshCredentials::trackWrite(&pending);
        QVERIFY(!manager.cleanupBlockedReason().isEmpty());QVERIFY(!manager.forgetPasswords());
        QVERIFY(write("#!/bin/sh\n[ \"$*\" = 'clear application pilon protocol ssh' ]\n"));
        pending.start("/nonexistent/pilon-test-process");QVERIFY(!pending.waitForStarted());
        QVERIFY(manager.cleanupBlockedReason().isEmpty());
        QVERIFY(manager.forgetPasswords());QVERIFY(manager.errorMessage().isEmpty());
        QVERIFY(SshCredentials::get("fixture","user").isEmpty());
        QVERIFY(write("#!/bin/sh\nexit 1\n"));QVERIFY(manager.forgetPasswords());
    }
    void shortFailurePreservesTheActualError() {
        qputenv("PILON_TEST_SSH_MODE", "failure");
        SshManager manager;
        manager.connectToHost("tester", "host.fixture.invalid");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.running(), 3000);
        QCOMPARE(manager.errorMessage(), "ssh: Could not resolve hostname unavailable.fixture.invalid: Name or service not known");
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
    void firstHostConfirmationIncludesIdentityAndFingerprint() {
        qputenv("PILON_TEST_SSH_MODE", "host");
        SshManager manager;
        manager.connectToHost("tester", "host.fixture.invalid");
        QTRY_VERIFY_WITH_TIMEOUT(!manager.promptText().isEmpty(), 3000);
        const auto details = manager.property("connectionText").toString();
        QVERIFY(details.contains("host.fixture.invalid (192.0.2.1)"));
        QVERIFY(details.contains("SHA256:fixtureFingerprint"));
        QVERIFY(manager.promptText().startsWith("Are you sure"));
        QVERIFY(!manager.promptSecret());
        manager.submitPrompt("yes");
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
};
QTEST_GUILESS_MAIN(SshTests)
#include "ssh_tests.moc"
