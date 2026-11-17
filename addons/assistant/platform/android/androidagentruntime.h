#pragma once
#include <QtGlobal>
#include <QCryptographicHash>

namespace AndroidAgentRuntime {
inline bool isUnmodifiedLegacyInstructions(const QByteArray &content)
{
    return QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex()
        == "234c99dbfadb95490ca375e4ec6dbf792f64e623256ec9ab3e9f5f99b618d508";
}
}

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include "../../../../src/core/workspace/pilonworkspace.h"

namespace AndroidAgentRuntime {
inline QString prepareEnvironment(QProcessEnvironment &environment, const QString &tools)
{
    const auto context = QNativeInterface::QAndroidApplication::context();
    const auto directory = QJniObject::fromString(tools);
    const auto response = QJniObject::callStaticObjectMethod("org/pilon/PilonAgentRuntime", "prepare",
        "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;", context.object(), directory.object<jstring>());
    const auto data = QJsonDocument::fromJson(response.toString().toUtf8()).object();
    if (!response.isValid() || data.isEmpty()) return QStringLiteral("The shared project runtime is unavailable.");
    if (data.contains("error")) return data.value("error").toString();
    const auto root = data.value("root").toString(), bin = data.value("bin").toString(), node = data.value("node").toString();
    if (!QDir::isAbsolutePath(root) || !QDir::isAbsolutePath(bin) || !QDir::isAbsolutePath(node)) return QStringLiteral("Invalid shared project runtime configuration.");
    environment.insert("PATH", bin + ':' + environment.value("PATH"));
    environment.insert("PYTHONHOME", root + "/python");
    environment.insert("PYTHONUTF8", "1");
    environment.insert("PILON_PYTHON", bin + "/python3");
    environment.insert("PYTHONPATH", tools + "/python-packages");
    environment.insert("PIP_TARGET", tools + "/python-packages");
    environment.insert("PIP_CACHE_DIR", tools + "/pip-cache");
    environment.insert("PILON_NODE", bin + "/node");
    environment.insert("PILON_AGENT_NODE_ROOT", node);
    environment.insert("PILON_NPM_CLI", root + "/npm/bin/npm-cli.js");
    environment.insert("PILON_NPX_CLI", root + "/npm/bin/npx-cli.js");
    environment.insert("PILON_VITE_CLI", node + "/node_modules/vite/bin/vite.js");
    environment.insert("NODE_PATH", node + "/node_modules");
    environment.insert("ESBUILD_BINARY_PATH", PilonWorkspace::packagedExecutable("esbuild"));
    environment.insert("NPM_CONFIG_BIN_LINKS", "false");
    environment.insert("NPM_CONFIG_USERCONFIG", data.value("npmrc").toString());
    environment.insert("NODE_EXTRA_CA_CERTS", environment.value("SSL_CERT_FILE"));
    environment.insert("LD_LIBRARY_PATH", QFileInfo(PilonWorkspace::packagedExecutable("node")).absolutePath());
    return {};
}
}
#endif
