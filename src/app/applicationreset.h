#pragma once
#include <QString>
#include <QVariantMap>

namespace ApplicationReset {
struct Locations { QString workspaceRoot; };
Locations locations();
bool check(const QVariantMap &keep, const QVariantMap &ownedPaths, const Locations &paths, QString *error);
bool removeSelected(const QVariantMap &keep, const QVariantMap &ownedPaths, const Locations &paths, QString *error);
bool reset(const QVariantMap &keep, const QVariantMap &ownedPaths, const Locations &paths, QString *error);
bool clearPreferences(QString *error);
bool recover(const Locations &paths, QString *error);
}
