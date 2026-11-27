#pragma once
#include <QString>

namespace PilonWorkspace {
QString configuredRoot();
QString addonsDirectory();
QString addonDataDirectory(const QString &id);
QString appDataDirectory();
QString assistantAgentDirectory();
QString publicDirectory(const QString &name);
QString bootstrapFile();
QString settingsFile();
QString modelsDirectory();
QString addonModelsDirectory(const QString &id);
QString runtimeDirectory();
#ifdef Q_OS_ANDROID
QString packagedExecutable(const QString &name);
#endif
QString artifactsDirectory();
QString currentPublicPath(const QString &root, const QString &path);
bool initializeStorage(QString *error);
bool relocateStorage(const QString &destination, QString *error);
bool validateRoot(const QString &path, QString *error);
bool persistRoot(const QString &path, QString *error);
bool moveWithoutReplacing(const QString &source, const QString &destination);
} // namespace PilonWorkspace
