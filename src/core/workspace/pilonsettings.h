#pragma once
#include "pilonworkspace.h"
#include <QSettings>

class PilonSettings : public QSettings {
  public:
    using QSettings::QSettings;
    explicit PilonSettings(QObject *parent = nullptr)
        : QSettings(PilonWorkspace::settingsFile(), QSettings::IniFormat, parent) {}
};
