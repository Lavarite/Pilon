#pragma once
#include <QMap>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <optional>

namespace PilonWidgetRoute {
inline constexpr quint64 maximumGeneration = 9007199254740991ULL;
inline bool addonId(const QString &value) {
    static const QRegularExpression pattern(R"(\A[a-z][a-z0-9-]*(?:\.[a-z][a-z0-9-]*)*\z)");
    return value.size() <= 128 && pattern.match(value).hasMatch();
}
inline bool widgetId(const QString &value) {
    static const QRegularExpression pattern(R"(\A[a-z][a-z0-9-]{0,63}\z)");
    return pattern.match(value).hasMatch();
}
inline bool id(const QString &value) {
    static const QRegularExpression pattern(R"(\A[A-Za-z0-9_-]{1,128}\z)");
    return pattern.match(value).hasMatch();
}
inline bool workspace(const QString &value) {
    static const QRegularExpression pattern(R"(\A[a-f0-9]{64}\z)");
    return pattern.match(value).hasMatch();
}
struct Action {
    QString addon, widget, workspaceId, actionId;
    quint64 generation;
};
inline std::optional<Action> parse(const QUrl &url) {
    if (!url.isValid() || url.toEncoded().size() > 2048 || url.scheme() != "pilon" ||
        url.authority(QUrl::FullyEncoded) != "widget" || url.hasFragment()) return {};
    const auto parts = url.path(QUrl::FullyEncoded).split('/');
    if (parts.size() != 3 || !parts[0].isEmpty() || !addonId(parts[1]) || !widgetId(parts[2])) return {};
    const auto pairs = QUrlQuery(url).queryItems(QUrl::FullyDecoded);
    if (pairs.size() != 3) return {};
    QMap<QString, QString> fields;
    for (const auto &pair : pairs) {
        if (fields.contains(pair.first)) return {};
        fields.insert(pair.first, pair.second);
    }
    const auto action = fields.value("action"), scope = fields.value("workspace"), number = fields.value("generation");
    static const QRegularExpression generationDigits(R"(\A[1-9][0-9]{0,15}\z)");
    if (!id(action) || !workspace(scope) || !generationDigits.match(number).hasMatch()) return {};
    const auto generation = number.toULongLong();
    if (generation > maximumGeneration) return {};
    return Action{parts[1], parts[2], scope, action, generation};
}
} // namespace PilonWidgetRoute
