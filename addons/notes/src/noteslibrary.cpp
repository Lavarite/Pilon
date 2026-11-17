#include "notesmanager.h"
#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <optional>

namespace {
bool numeric(const QVariant &value) {
    const auto type = value.metaType().id();
    return type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong
        || type == QMetaType::ULongLong || type == QMetaType::Double || type == QMetaType::Float;
}
std::optional<QVariantMap> validatedView(const QVariantMap &note, const QVariantMap &state) {
    for (const auto *name : {"zoom", "x", "y"})
        if (!numeric(state.value(name)) || !std::isfinite(state.value(name).toDouble())) return {};
    const auto zoom = state.value("zoom").toDouble(), x = state.value("x").toDouble(), y = state.value("y").toDouble();
    if (zoom < .25 || zoom > 4 || x < 0 || y < 0 || !std::isfinite(x * zoom) || !std::isfinite(y * zoom)
        || state.value("editing").metaType().id() != QMetaType::Bool) return {};
    const auto revisionValue = state.value("revision");
    if (!numeric(revisionValue)) return {};
    if (revisionValue.metaType().id() == QMetaType::Double || revisionValue.metaType().id() == QMetaType::Float) {
        const auto value = revisionValue.toDouble();
        if (!std::isfinite(value) || value < 0 || value > 9007199254740991. || std::trunc(value) != value) return {};
    }
    bool validRevision = false; const auto revision = revisionValue.toLongLong(&validRevision);
    if (!validRevision || revision < 0) return {};
    const auto rawSelection = state.value("selection");
    QStringList selection;
    if (rawSelection.metaType().id() == QMetaType::QStringList) selection = rawSelection.toStringList();
    else if (rawSelection.metaType().id() == QMetaType::QVariantList) {
        for (const auto &value : rawSelection.toList()) {
            if (value.metaType().id() != QMetaType::QString) return {};
            selection.append(value.toString());
        }
    } else return {};
    QStringList accepted;
    if (revision == note.value("inkRevision").toLongLong()) {
        QSet<QString> elementKeys, seen;
        for (const auto &element : note.value("elements").toList()) {
            const auto id = element.toMap().value("id").toString();
            if (!id.isEmpty()) elementKeys.insert("e" + id);
        }
        static const QRegularExpression strokeKey("^s(0|[1-9][0-9]*)$");
        const auto count = static_cast<qulonglong>(note.value("strokes").toList().size());
        for (const auto &key : selection) {
            bool valid = elementKeys.contains(key);
            if (!valid && strokeKey.match(key).hasMatch()) {
                bool ok = false; const auto index = key.mid(1).toULongLong(&ok); valid = ok && index < count;
            }
            if (valid && !seen.contains(key)) { seen.insert(key); accepted.append(key); }
        }
    }
    return QVariantMap{{"zoom", zoom}, {"x", x}, {"y", y}, {"selection", accepted}, {"revision", revision}, {"editing", state.value("editing").toBool()}};
}
}

QVariantMap NotesManager::noteViewState(const QString &id) const {
    if (!m_notes.contains(id)) return {};
    const auto note = m_notes.value(id);
    auto state = validatedView(note, m_library.value("views").toMap().value(id).toMap()).value_or(QVariantMap{});
    // A stale stored selection stays empty; only the returned capture revision advances.
    state["revision"] = note.value("inkRevision").toLongLong();
    return state;
}
bool NotesManager::setNoteViewState(const QString &id, const QVariantMap &state) {
    if (!m_active || m_loading || !m_notes.contains(id)) return false;
    const auto normalized = validatedView(m_notes.value(id), state);
    if (!normalized) { error(tr("Invalid note view state.")); return false; }
    auto library = m_library, views = library.value("views").toMap();
    if (views.value(id).toMap() == *normalized) return true;
    views[id] = *normalized; library["views"] = views;
    return saveLibrary(library);
}

bool NotesManager::saveLibrary(const QVariantMap &library) {
    if (!writeJson(".library.json", library)) return false;
    m_library = library; emit itemsChanged(); return true;
}
void NotesManager::rememberOpen(const QString &id) {
    auto library = m_library, opened = library.value("opened").toMap();
    opened[id] = QDateTime::currentMSecsSinceEpoch(); library["opened"] = opened; saveLibrary(library);
}
