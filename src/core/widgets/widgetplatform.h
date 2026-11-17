#pragma once
#include <QString>
#include <QVariantList>

namespace HomeWidgets {
QString directory();
bool available();
bool canRequestAdd();
bool requestAdd(const QString &addonId, const QString &widgetId);
void refresh();
QVariantList activeProviders();
}
