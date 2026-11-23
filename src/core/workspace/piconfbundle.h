#pragma once

#include "piconfdocument.h"
#include <QUrl>

namespace PiconfBundle {
// Only this envelope is transferred. The workspace's native stores, packages
// and rollback journal are never pack members.
bool save(const QUrl &destination, const PiconfDocument &document, QString &error);
bool open(const QUrl &source, PiconfDocument &document, QString &error);
}
