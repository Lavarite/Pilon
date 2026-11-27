#include "assistanthubcredentials.h"

#include <QCoreApplication>
#ifdef Q_OS_ANDROID
#include "../../../src/platform/android/androidcredentials.h"
#else
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#endif
#include <algorithm>

namespace {
constexpr qsizetype maximumTokenSize = 2048;

bool validToken(const QByteArray &token)
{
    return !token.isEmpty() && token.size() <= maximumTokenSize
        && std::all_of(token.begin(), token.end(), [](unsigned char ch) { return ch > 32 && ch < 127; });
}

AssistantHubCredentials::Result failure(const char *message)
{
    return {false, {}, QCoreApplication::translate("AssistantHubCredentials", message)};
}

AssistantHubCredentials::Result request(const QString &action, const QByteArray &token = {})
{
    QByteArray stored;
#ifdef Q_OS_ANDROID
    const auto result = AndroidCredentials::request(action,
        {{"application", "pilon"}, {"provider", "huggingface"}}, token);
    if (!result.ok)
        return failure(QT_TRANSLATE_NOOP("AssistantHubCredentials", "The Android credential store could not complete the request."));
    if (action == "lookup" && result.found)
        stored = result.secret;
#else
    const auto tool = QStandardPaths::findExecutable("secret-tool");
    if (tool.isEmpty())
        return failure(QT_TRANSLATE_NOOP("AssistantHubCredentials", "Install secret-tool (libsecret) to save Hugging Face credentials."));

    QStringList arguments{action};
    if (action == "store")
        arguments.append("--label=Pilon Hugging Face account");
    arguments.append({"application", "pilon", "provider", "huggingface"});

    QProcess process;
    bool hadDiagnostic = false;
    bool oversized = false;
    QObject::connect(&process, &QProcess::readyReadStandardOutput, &process, [&] {
        const auto output = process.readAllStandardOutput();
        if (stored.size() + output.size() > maximumTokenSize + 1) {
            oversized = true;
            process.kill();
        } else {
            stored += output;
        }
    });
    QObject::connect(&process, &QProcess::readyReadStandardError, &process, [&] {
        hadDiagnostic |= !process.readAllStandardError().isEmpty();
    });
    process.start(tool, arguments);
    if (!process.waitForStarted(3000))
        return failure(QT_TRANSLATE_NOOP("AssistantHubCredentials", "The system keyring could not be opened."));
    if (action == "store")
        process.write(token);
    process.closeWriteChannel();
    if (process.state() != QProcess::NotRunning && !process.waitForFinished(10000)) {
        process.kill();
        process.waitForFinished(1000);
        return failure(QT_TRANSLATE_NOOP("AssistantHubCredentials", "The system keyring did not respond. Unlock it and try again."));
    }
    // A failed clear can mean the matching item is still locked.
    const bool absent = action == "lookup" && process.exitCode() == 1 && !hadDiagnostic && stored.isEmpty();
    if (oversized || process.exitStatus() != QProcess::NormalExit || (process.exitCode() != 0 && !absent))
        return failure(QT_TRANSLATE_NOOP("AssistantHubCredentials", "The system keyring request failed. Unlock it and try again."));
    if (action != "lookup")
        stored.clear();
    if (stored.endsWith('\n'))
        stored.chop(1);
#endif
    if (!stored.isEmpty() && !validToken(stored))
        return failure(QT_TRANSLATE_NOOP("AssistantHubCredentials", "The saved Hugging Face access token is invalid. Sign in again."));
    return {true, stored, {}};
}
}

AssistantHubCredentials::Result AssistantHubCredentials::lookup() { return request("lookup"); }

AssistantHubCredentials::Result AssistantHubCredentials::store(const QByteArray &token)
{
    if (!validToken(token))
        return failure(QT_TRANSLATE_NOOP("AssistantHubCredentials", "Enter a valid Hugging Face access token."));
    return request("store", token);
}

AssistantHubCredentials::Result AssistantHubCredentials::clear() { return request("clear"); }
