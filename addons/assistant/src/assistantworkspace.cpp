#include "assistantmanager.h"
#include "../../../src/core/workspace/pilonworkspace.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <csignal>
#include <unistd.h>
#include <utility>
#ifdef Q_OS_ANDROID
#include "../../../src/platform/android/androidplatformtrust.h"
#include "../platform/android/androidagentruntime.h"
#include <QJniEnvironment>
#include <QJniObject>

namespace {
QHash<qint64, QPointer<AssistantManager>> browserRequests;
qint64 nextBrowserRequest = 0;
}
#endif

QString AssistantManager::workspaceRoot() const
{
    if (!qEnvironmentVariable("PILON_AGENT_DIR").isEmpty())
        return QDir::cleanPath(qEnvironmentVariable("PILON_AGENT_DIR"));
    if (qEnvironmentVariableIsSet("PILON_EPHEMERAL_CHAT"))
        return m_sessionDirectory.path() + "/agent";
    return PilonWorkspace::assistantAgentDirectory();
}

QString AssistantManager::toolsRoot() const { return workspaceRoot() + "/tools"; }

bool AssistantManager::initializeWorkspace(const QString &legacyWorkspace)
{
    if (!QDir().mkpath(toolsRoot()) || (m_projectId.isEmpty() && !QDir().mkpath(workingDirectory()))) return false;
#ifdef Q_OS_ANDROID
    Q_UNUSED(legacyWorkspace);
    auto environment = QProcessEnvironment::systemEnvironment();
    if (!AndroidPlatformTrust::exportCertificates(environment).isEmpty()) return false;
    if (!AndroidAgentRuntime::prepareEnvironment(environment, toolsRoot()).isEmpty()) return false;
#endif
#ifndef Q_OS_ANDROID
    QString legacy = legacyWorkspace;
    if (legacy.isEmpty() && qEnvironmentVariableIsEmpty("PILON_AGENT_DIR") && !qEnvironmentVariableIsSet("PILON_EPHEMERAL_CHAT"))
        legacy = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../AgentFiles");
    const bool inherit = !legacy.isEmpty() && !QFileInfo(legacy).isSymLink();
#endif
    const QString source = ":/qt/qml/Pilon/agent-tools/";
    for (const QString &name : {QString("AGENTS.md"), QString("pyproject.toml"), QString("uv.lock"), QString("browser_worker.py"), QString("create_artifact.py")}) {
        const QString destination = (name == "AGENTS.md" ? workspaceRoot() : toolsRoot()) + '/' + name;
        const bool userFile = name != "browser_worker.py" && name != "create_artifact.py";
        // Preserve the user's instructions and packages when upgrading Pilon.
        if (userFile && QFileInfo::exists(destination)) {
#ifdef Q_OS_ANDROID
            QFile previous(destination);
            if (name != "AGENTS.md" || !previous.open(QIODevice::ReadOnly)
                || !AndroidAgentRuntime::isUnmodifiedLegacyInstructions(previous.readAll())) continue;
#else
            continue;
#endif
        }
#ifndef Q_OS_ANDROID
        if (inherit && userFile && (name == "AGENTS.md" || !QFileInfo(legacy + "/tools").isSymLink())) {
            const QFileInfo former(legacy + (name == "AGENTS.md" ? "/" : "/tools/") + name);
            if (former.isFile() && !former.isSymLink()) {
                if (!QFile::copy(former.filePath(), destination) && !QFileInfo::exists(destination)) return false;
                continue;
            }
        }
#endif
        QFile input(source + name);
        if (!input.open(QIODevice::ReadOnly)) return false;
        const auto content = input.readAll();
        QFile existing(destination);
        if (existing.open(QIODevice::ReadOnly) && existing.readAll() == content) continue;
        QSaveFile output(destination);
        if (!output.open(QIODevice::WriteOnly) || output.write(content) != content.size() || !output.commit()) return false;
    }
    return true;
}

QProcessEnvironment AssistantManager::toolEnvironment() const
{
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("PILON_AGENT_DIR", workspaceRoot());
    environment.insert("PILON_AGENT_TOOLS", toolsRoot());
#ifdef Q_OS_ANDROID
    environment.insert("SHELL", "/system/bin/sh");
    environment.insert("PATH", QStringLiteral("/system/bin")
        + (environment.value("PATH").isEmpty() ? QString{} : ':' + environment.value("PATH")));
    environment.insert("SSL_CERT_FILE", AndroidPlatformTrust::certificatePath());
    for (const auto *name : {"NODE_PATH", "UV_PROJECT", "UV_PROJECT_ENVIRONMENT", "VIRTUAL_ENV",
            "PLAYWRIGHT_BROWSERS_PATH"}) environment.remove(name);
    AndroidAgentRuntime::prepareEnvironment(environment, toolsRoot());
#else
    environment.insert("NODE_PATH", toolsRoot() + "/node/node_modules");
    environment.insert("UV_PROJECT", toolsRoot());
    environment.insert("UV_PROJECT_ENVIRONMENT", toolsRoot() + "/.venv");
    environment.insert("VIRTUAL_ENV", toolsRoot() + "/.venv");
    environment.insert("PLAYWRIGHT_BROWSERS_PATH", toolsRoot() + "/browsers");
    environment.insert("PATH", toolsRoot() + "/.venv/bin:" + environment.value("PATH"));
#endif
    return environment;
}

void AssistantManager::initializeBrowser()
{
#ifdef Q_OS_ANDROID
    static const bool registered = [] {
        QJniEnvironment environment;
        return environment.registerNativeMethods("org/pilon/PilonBrowser", {{"completed", "(JLjava/lang/String;)V",
            reinterpret_cast<void *>(+[](JNIEnv *, jclass, jlong request, jstring value) {
                const auto result = QJniObject(value).toString().toUtf8();
                // Java callbacks arrive on Android's UI thread; assistant state belongs to Qt.
                QMetaObject::invokeMethod(qApp, [request, result] {
                    const auto manager = browserRequests.take(request);
                    if (!manager || manager->m_browserRequestId != request) return;
                    manager->m_browserRequestId = 0;
                    manager->m_browserRequest = {};
                    if (!manager->m_busy || manager->m_currentTool.isEmpty()
                        || manager->m_browserGeneration != manager->m_generation) return;
                    QJsonParseError error;
                    const auto document = QJsonDocument::fromJson(result, &error);
                    manager->finishTool(error.error == QJsonParseError::NoError && document.isObject()
                        ? document.object() : QJsonObject{{"error", "Invalid response from the browser."}});
                }, Qt::QueuedConnection);
            })}});
    }();
    m_browserReady = registered;
#else
    m_browserDeadline.setSingleShot(true);
    connect(&m_browserDeadline, &QTimer::timeout, this, [this] {
        failBrowserRequest(tr("The browser did not finish in time. Try again or use a different source."));
    });
    m_browserProcess.setChildProcessModifier([] { ::setsid(); });
    connect(&m_browserProcess, &QProcess::readyReadStandardError, this, [this] {
        m_browserLog = (m_browserLog + m_browserProcess.readAllStandardError()).right(8192);
    });
    connect(&m_browserProcess, &QProcess::readyReadStandardOutput, this, [this] {
        m_browserBuffer += m_browserProcess.readAllStandardOutput();
        if (m_browserBuffer.size() > 4 * 1024 * 1024) {
            failBrowserRequest(tr("The browser returned more data than Pilon can process."));
            return;
        }
        while (m_browserBuffer.contains('\n')) {
            const auto end = m_browserBuffer.indexOf('\n');
            const auto line = m_browserBuffer.first(end);
            m_browserBuffer.remove(0, end + 1);
            const auto event = QJsonDocument::fromJson(line).object();
            if (event.value("ready").toBool()) {
                m_browserReady = true;
                if (!m_browserRequest.isEmpty())
                    sendBrowserRequest();
            } else if (event.value("result").isObject() && !m_browserRequest.isEmpty()) {
                m_browserDeadline.stop();
                m_browserRequest = {};
                if (m_busy && !m_currentTool.isEmpty()) finishTool(event.value("result").toObject());
            }
        }
    });
    connect(&m_browserProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart || error == QProcess::WriteError || error == QProcess::ReadError)
            failBrowserRequest(m_browserProcess.errorString());
    });
    connect(&m_browserProcess, &QProcess::finished, this, [this] {
        m_browserReady = false;
        m_browserDeadline.stop();
        if (m_browserRequest.isEmpty()) return;
        m_browserRequest = {};
        if (m_busy) finishTool({{"error", "The browser stopped."}, {"output", QString::fromUtf8(m_browserLog)}});
    });
#endif
}

#ifndef Q_OS_ANDROID
void AssistantManager::sendBrowserRequest()
{
    const auto message = QJsonDocument(m_browserRequest).toJson(QJsonDocument::Compact) + '\n';
    m_browserDeadline.start(25000);
    if (m_browserProcess.write(message) != message.size())
        failBrowserRequest(tr("Unable to send the browser request."));
}

void AssistantManager::failBrowserRequest(const QString &error)
{
    const bool pending = !m_browserRequest.isEmpty();
    stopBrowser();
    if (pending && m_busy && !m_currentTool.isEmpty()) finishTool({{"error", error}});
}
#endif

bool AssistantManager::runBrowserTool(const QString &name, const QJsonObject &args)
{
    if (name != "browser_search" && name != "browser_open") return false;
    if (QJsonDocument(args).toJson(QJsonDocument::Compact).size() > 32768) {
        finishTool({{"error", "The browser request exceeds 32 KiB."}});
        return true;
    }
    if (!initializeWorkspace()) {
        finishTool({{"error", "Unable to prepare the agent workspace."}});
        return true;
    }
    m_browserRequest = {{"name", name}, {"args", args}};
    m_toolExecuting = true;
    setStatus(tr("Using the browser…"));
#ifdef Q_OS_ANDROID
    if (!m_browserReady) {
        m_browserRequest = {};
        finishTool({{"error", "The Android browser is unavailable."}});
        return true;
    }
    m_browserRequestId = ++nextBrowserRequest;
    m_browserGeneration = m_generation;
    browserRequests.insert(m_browserRequestId, this);
    const auto request = m_browserRequestId;
    const auto message = QString::fromUtf8(QJsonDocument(m_browserRequest).toJson(QJsonDocument::Compact));
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([request, message] {
        const QJniObject context = QNativeInterface::QAndroidApplication::context();
        const auto json = QJniObject::fromString(message);
        QJniObject::callStaticMethod<void>("org/pilon/PilonBrowser", "request",
            "(Landroid/content/Context;JLjava/lang/String;)V", context.object(), jlong(request), json.object<jstring>());
    });
#else
    if (m_browserReady) {
        sendBrowserRequest();
    } else if (m_browserProcess.state() == QProcess::NotRunning) {
        m_browserBuffer.clear();
        m_browserLog.clear();
        m_browserProcess.setWorkingDirectory(workspaceRoot());
        m_browserProcess.setProcessEnvironment(toolEnvironment());
        m_browserDeadline.start(120000);
        m_browserProcess.start("uv", {"run", "--project", toolsRoot(), "python", toolsRoot() + "/browser_worker.py"});
    }
#endif
    return true;
}

void AssistantManager::stopBrowser()
{
    m_browserRequest = {};
#ifdef Q_OS_ANDROID
    const auto request = std::exchange(m_browserRequestId, 0);
    browserRequests.remove(request);
    if (request) QNativeInterface::QAndroidApplication::runOnAndroidMainThread([request] {
        QJniObject::callStaticMethod<void>("org/pilon/PilonBrowser", "cancel", "(J)V", jlong(request));
    });
#else
    m_browserReady = false;
    m_browserDeadline.stop();
    m_browserBuffer.clear();
    if (m_browserProcess.state() == QProcess::NotRunning) return;
    if (m_browserProcess.processId() > 0) ::kill(-m_browserProcess.processId(), SIGKILL);
    m_browserProcess.kill();
    m_browserProcess.waitForFinished(1000);
#endif
}
