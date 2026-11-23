#include "applicationservice.h"
#include "../core/workspace/piconfbundle.h"
#include "../core/workspace/piconfpreferences.h"
#include "../core/workspace/piconftransaction.h"
#include "../core/workspace/pilonsettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace {
// The registry already diagnoses missing, incompatible and cyclic dependencies.
// Its public records provide the installed graph even for disabled packages.
bool checkEnabled(const QVariantMap &declarations, const QVariantList &installed, QString &error)
{
    QMap<QString, bool> enabled;
    for (const auto &entry : installed) {
        const auto row = entry.toMap();
        const auto id = row.value("id").toString();
        const auto desired = declarations.value(id).toMap();
        enabled[id] = desired.value("enabled", row.value("configuredEnabled")).toBool();
        if (desired.isEmpty()) continue;
        if (enabled[id] && !row.value("configuredEnabled").toBool()) {
            const auto problem = row.value("error").toString().isEmpty()
                ? row.value("blockedReason").toString() : row.value("error").toString();
            if (!problem.isEmpty()) { error = QObject::tr("Cannot enable %1: %2").arg(id, problem); return false; }
        }
        for (const auto &pin : {QStringLiteral("version"), QStringLiteral("sha256")}) {
            const auto expected = desired.value(pin).toString();
            const auto sensitivity = pin == "sha256" ? Qt::CaseInsensitive : Qt::CaseSensitive;
            if (!expected.isEmpty() && expected.compare(row.value(pin).toString(), sensitivity) != 0) {
                error = QObject::tr("The installed %1 does not match its configured %2. Install the matching package before importing.").arg(id, pin);
                return false;
            }
        }
    }
    for (const auto &entry : installed) {
        const auto row = entry.toMap();
        const auto id = row.value("id").toString();
        if (!enabled.value(id)) continue;
        for (const auto &dependency : row.value("dependencies").toList()) {
            const auto provider = dependency.toMap().value("id").toString();
            if (enabled.contains(provider) && !enabled.value(provider)) {
                error = QObject::tr("%1 requires %2. Enable its dependency or disable %1 in the configuration.").arg(id, provider);
                return false;
            }
        }
    }
    return true;
}

QString displayValue(const QVariant &value)
{
    if (!value.isValid()) return QObject::tr("Default");
    if (value.typeId() == QMetaType::Bool) return value.toBool() ? QObject::tr("On") : QObject::tr("Off");
    if (value.typeId() == QMetaType::QVariantList || value.typeId() == QMetaType::QStringList)
        return QObject::tr("%n item(s)", nullptr, value.toList().size());
    const QString text = value.toString().simplified();
    return text.size() > 70 ? text.left(69) + QChar(0x2026) : text;
}

void describeChanges(const QString &prefix, const QVariantMap &before, const QVariantMap &after, QStringList &changes)
{
    QMap<QString, QVariant> keys(before);
    keys.insert(after);
    for (auto i = keys.cbegin(); i != keys.cend(); ++i) {
        const auto name = i.key();
        const auto left = before.value(name), right = after.value(name);
        if (left == right && before.contains(name) == after.contains(name)) continue;
        const auto key = prefix.isEmpty() ? name : prefix + '.' + name;
        if ((left.metaType().id() == QMetaType::QVariantMap || right.metaType().id() == QMetaType::QVariantMap)
            && (!left.isValid() || left.typeId() == QMetaType::QVariantMap)
            && (!right.isValid() || right.typeId() == QMetaType::QVariantMap)) {
            describeChanges(key, left.toMap(), right.toMap(), changes);
        } else if (!after.contains(name)) {
            changes.append(QObject::tr("%1 — reset to default").arg(key));
        } else {
            const auto was = displayValue(left), now = displayValue(right);
            changes.append(was == now ? key : QObject::tr("%1: %2 → %3").arg(key, was, now));
        }
    }
}

void deviceReferences(const QVariant &value, const QString &key, QStringList &references)
{
    if (references.size() >= 24) return;
    if (value.typeId() == QMetaType::QVariantMap) {
        const auto map = value.toMap();
        for (auto i = map.cbegin(); i != map.cend(); ++i)
            deviceReferences(i.value(), i.key(), references);
    } else if (value.typeId() == QMetaType::QVariantList || value.typeId() == QMetaType::QStringList) {
        for (const auto &item : value.toList()) deviceReferences(item, key, references);
    } else if (value.typeId() == QMetaType::QString) {
        const auto string = value.toString();
        const auto url = QUrl(string);
        QString label;
        if (QDir::isAbsolutePath(string) || url.isLocalFile() || url.scheme() == "content")
            label = string;
        else if ((key == "credentialUsername" || key == "credentialUrl" || key == "sshKey") && !string.isEmpty())
            label = QObject::tr("Local credential reference (%1)").arg(key);
        if (!label.isEmpty() && !references.contains(label)) references << label;
    }
}
}

QVariantMap ApplicationService::configuredAddons() const
{
    PiconfDocument document;
    QString error;
    return document.load(workspaceRoot(), error) ? document.values().value("addons").toMap() : QVariantMap{};
}

QVariant ApplicationService::prepareConfiguredAddon(const QString &id)
{
    QString error;
    if (!PiconfPreferences::instance().prepareInstalled(id, error)) return error;
    return true;
}

bool ApplicationService::exportConfiguration(const QUrl &destination)
{
    QString error;
    PiconfDocument document;
    if (!PiconfPreferences::instance().flush(error) || !document.load(workspaceRoot(), error)
        || !PiconfBundle::save(destination, document, error)) {
        reportError(error); return false;
    }
    reportError({}); return true;
}

QVariantMap ApplicationService::previewConfiguration(const QUrl &source, const QVariantList &installed)
{
    cancelConfigurationImport();
    auto pending = std::make_unique<PendingConfiguration>();
    pending->root = workspaceRoot();
    pending->installed = installed;
    auto &preferences = PiconfPreferences::instance();
    QString error;
    if (!PiconfBundle::open(source, pending->incoming, error)
        || !preferences.validate(pending->incoming, error)) {
        reportError(error); return {};
    }
    // A valid backup can replace a graph that failed to load at startup. Snapshot each
    // destination so the same conflict checks protect a repair and a normal import.
    PiconfDocument current;
    pending->repairing = !preferences.active();
    if (pending->repairing) {
        if (!PiconfTransaction::snapshotEditable(pending->root, pending->incoming.files().keys(), pending->expected, error)) {
            reportError(error); return {};
        }
    } else {
        if (!preferences.flush(error) || !current.load(pending->root, error)) { reportError(error); return {}; }
        pending->expected = current.files();
    }
    const auto declarations = pending->incoming.values().value("addons").toMap();
    if (!checkEnabled(declarations, installed, error)) { reportError(error); return {}; }
    QSet<QString> present;
    for (const auto &entry : installed) present.insert(entry.toMap().value("id").toString());
    QStringList missing, changes, references;
    for (auto i = declarations.cbegin(); i != declarations.cend(); ++i) {
        if (present.contains(i.key())) continue;
        const auto origin = i.value().toMap().value("source").toString();
        missing.append(origin.isEmpty() ? i.key() : i.key() + " (" + origin + ')');
    }
    // Compare resolved values, including changes originating in shared variable files.
    for (auto i = pending->incoming.files().cbegin(); i != pending->incoming.files().cend(); ++i) {
        const bool exists = pending->expected.contains(i.key());
        if (!pending->repairing && !exists && QFileInfo::exists(QDir(pending->root).filePath(i.key()))) {
            reportError(tr("Import would replace an unreferenced file: %1").arg(i.key())); return {};
        }
        const auto prior = exists && !pending->repairing ? current.values(i.key()) : QVariantMap{};
        describeChanges(i.key() == PiconfDocument::MainPath ? QString{} : i.key(),
                        prior, pending->incoming.values(i.key()), changes);
        deviceReferences(pending->incoming.values(i.key()), {}, references);
    }
    QStringList details;
    if (pending->repairing) details << tr("These files will be restored from the bundle:") << pending->incoming.files().keys();
    if (!missing.isEmpty()) details << tr("Missing add-ons") << missing;
    if (!references.isEmpty()) details << tr("Check on this device") << references;
    if (!changes.isEmpty()) {
        if (!details.isEmpty()) details << QString{};
        details << changes.mid(0, 80);
        if (changes.size() > 80) details << tr("%1 more settings").arg(changes.size() - 80);
    }
    const bool different = pending->repairing || pending->expected != pending->incoming.files();
    if (different && details.isEmpty())
        details << tr("Only configuration text or file references changed.");
    const auto summary = pending->repairing ? tr("Restore the unreadable configuration from this bundle and reopen Pilon?")
        : different ? tr("Import these settings and reopen Pilon?") : tr("This configuration already matches this workspace.");
    m_configuration = std::move(pending);
    reportError({});
    return {{"summary", summary},
            {"details", details.join('\n')}, {"different", different}};
}

void ApplicationService::cancelConfigurationImport() { m_configuration.reset(); }

bool ApplicationService::reconcileConfiguredStartup(bool previous, QString &error)
{
#ifdef Q_OS_ANDROID
    Q_UNUSED(previous)
    Q_UNUSED(error)
#else
    PilonSettings settings;
    const bool desired = settings.value("app/backgroundEnabled", false).toBool();
    if (desired != previous && !configureStartup(desired)) {
        error = m_error;
        settings.setValue("app/backgroundEnabled", previous);
        settings.sync();
        QString syncError;
        if (!PiconfPreferences::instance().flush(syncError)) error += '\n' + syncError;
        return false;
    }
#endif
    return true;
}

bool ApplicationService::confirmConfigurationImport(const QVariantList &installed)
{
    QString error;
    if (!m_configuration || m_configuration->root != workspaceRoot()) {
        reportError(tr("Select the configuration bundle again.")); return false;
    }
    if (installed != m_configuration->installed || !checkEnabled(m_configuration->incoming.values().value("addons").toMap(), installed, error)) {
        reportError(error.isEmpty() ? tr("Installed add-ons changed. Review the import again.") : error); return false;
    }
    m_quitCancelled = false;
    emit quitRequested();
    if (m_quitCancelled) return false;
    QCoreApplication::exit(ConfigurationExitCode);
    return true;
}

bool ApplicationService::finishConfigurationImport(QString &error)
{
    auto pending = std::move(m_configuration);
    if (!pending || pending->root != workspaceRoot()) {
        error = tr("The configuration import no longer belongs to this workspace."); return false;
    }
    auto &preferences = PiconfPreferences::instance();
    auto expected = pending->expected;
    auto updated = pending->incoming.files();
    if ((!pending->repairing && !preferences.flush(error)) || !preferences.prepareImported(pending->incoming, expected, updated, error)) return false;
#ifndef Q_OS_ANDROID
    const bool startup = pending->incoming.values().value("application").toMap().value("backgroundEnabled", false).toBool();
    const bool changeStartup = startup != backgroundEnabled();
    if (changeStartup && !configureStartup(startup)) { error = m_error; return false; }
#endif
    if (!PiconfTransaction::commit(pending->root, expected, updated, error)) {
#ifndef Q_OS_ANDROID
        if (changeStartup && !configureStartup(backgroundEnabled())) error += '\n' + m_error;
#endif
        return false;
    }
    return true;
}
