#include "assistantmanager.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStringDecoder>
#include <QTextDocumentFragment>
#include <QUrlQuery>
#include <cmath>

namespace {
constexpr qint64 MaxResearchBytes = 8 * 1024 * 1024;
const QString InsufficientSummary = QStringLiteral("Summary budget too small; request more characters.");
QJsonObject researchFunction(const QJsonObject &call)
{
    auto function = call.value("function").toObject();
    if (function.value("name") == "call_tool") {
        const auto args = QJsonDocument::fromJson(function.value("arguments").toString().toUtf8()).object();
        function = {{"name", args.value("name")},
            {"arguments", QString::fromUtf8(QJsonDocument(args.value("arguments").toObject()).toJson(QJsonDocument::Compact))}};
    }
    if (function.value("name").toString().startsWith("pilon__"))
        function.insert("name", function.value("name").toString().sliced(7));
    return function;
}

bool webUrl(const QUrl &url)
{
    return url.isValid() && (url.scheme() == "https" || url.scheme() == "http")
        && !url.host().isEmpty() && url.userInfo().isEmpty();
}

QString plainText(QString html)
{
    html.remove(QRegularExpression("<(script|style|noscript|svg)\\b[^>]*>.*?</\\1\\s*>",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption));
    html.remove(QRegularExpression("<!--.*?-->", QRegularExpression::DotMatchesEverythingOption));
    return QTextDocumentFragment::fromHtml(html).toPlainText().trimmed();
}
}

namespace {
bool researchTool(const QString &name)
{
    return name == "browser_search" || name == "browser_open" || name == "read_webpage"
        || name == "search_wikipedia" || name == "search_papers";
}

QSet<QString> summaryWords(const QString &text)
{
    static const QSet<QString> stopWords = {"about", "after", "also", "been", "before", "between",
        "from", "have", "into", "more", "only", "other", "over", "some", "than", "that", "their",
        "them", "there", "these", "they", "this", "through", "were", "what", "when", "which",
        "while", "will", "with", "would", "your"};
    static const QRegularExpression words("[\\p{L}][\\p{L}\\p{N}_-]{3,}");
    QSet<QString> result;
    auto matches = words.globalMatch(text.toCaseFolded());
    while (matches.hasNext()) {
        const auto word = matches.next().captured();
        if (!stopWords.contains(word)) result.insert(word);
    }
    return result;
}

QString researchSummary(const QString &content, const QString &focus, int budget)
{
    if (budget == 0) return {};
    // Extract complete source sentences across the page. No model round-trip,
    // invented paraphrases, or detached/rounded numbers enter the evidence.
    struct Sentence { QString text; QSet<QString> words; bool number; double score = 0; };
    QList<Sentence> sentences;
    QHash<QString, int> frequency;
    QSet<QString> seen;
    static const QRegularExpression boundaries("[\\r\\n]+|(?<=[.!?])\\s+(?=[\\p{Lu}\\p{N}])");
    static const QRegularExpression digits("\\p{N}");
    static const QRegularExpression conclusions("\\b(summary|conclusion|finding|found|result|total|overall|increased|decreased)\\b",
                                               QRegularExpression::CaseInsensitiveOption);
    const int maximum = qBound(64, (budget - 80) / 2, 600);
    for (const QString &part : content.split(boundaries, Qt::SkipEmptyParts)) {
        const auto line = part.simplified();
        qsizetype position = 0;
        // Bound exceptionally long sentences and minified text, keeping word
        // boundaries where possible and marking incomplete source sentences.
        while (position < line.size()) {
            int length = qMin<qsizetype>(line.size() - position, maximum);
            if (position + length < line.size()) {
                const auto space = QStringView(line).sliced(position, length).lastIndexOf(' ');
                if (space > maximum / 2) length = space;
            }
            QString sentence = line.mid(position, length).trimmed();
            position += length;
            const bool continued = position < line.size();
            if (continued) sentence += QStringLiteral(" …");
            const QString key = sentence.toCaseFolded();
            if (sentence.isEmpty() || seen.contains(key)) continue;
            seen.insert(key);
            const auto words = summaryWords(sentence);
            for (const auto &word : words) ++frequency[word];
            sentences.append({sentence, words, digits.match(sentence).hasMatch()});
        }
    }
    const auto focused = summaryWords(focus);
    for (qsizetype index = 0; index < sentences.size(); ++index) {
        auto &sentence = sentences[index];
        double topical = 0;
        for (const auto &word : sentence.words) {
            topical += std::log1p(frequency.value(word));
            if (focused.contains(word)) sentence.score += 12;
        }
        sentence.score += topical / std::sqrt(qMax<qsizetype>(1, sentence.words.size()));
        if (index < 3) sentence.score += 3 - index;
        if (conclusions.match(sentence.text).hasMatch()) sentence.score += 5;
    }
    QList<qsizetype> ranked;
    for (qsizetype index = 0; index < sentences.size(); ++index) ranked.append(index);
    std::stable_sort(ranked.begin(), ranked.end(), [&](qsizetype a, qsizetype b) {
        return sentences[a].score > sentences[b].score;
    });
    QSet<qsizetype> selected;
    QStringList sections;
    int remaining = budget;
    const auto select = [&](bool numbers, const QString &heading, int available) {
        QList<qsizetype> chosen;
        int used = heading.size();
        for (const auto index : ranked) {
            const auto &sentence = sentences[index];
            if (selected.contains(index) || (numbers && !sentence.number)) continue;
            if (sentence.text.size() + 3 + used > available) continue;
            chosen.append(index);
            selected.insert(index);
            used += sentence.text.size() + 3;
            if (chosen.size() >= 8) break;
        }
        if (chosen.isEmpty()) return;
        std::sort(chosen.begin(), chosen.end()); // Keep the source's order/context.
        QString section = heading;
        for (const auto index : chosen) section += "\n- " + sentences[index].text;
        sections.append(section);
        remaining -= section.size() + 2;
    };
    // Reserve space for figures first, so a lengthy introduction cannot hide
    // measurements, prices or dates near the end of a document.
    select(true, "Key figures (source sentences):", budget / 2);
    select(false, "Main points (source sentences):", remaining);
    std::reverse(sections.begin(), sections.end());
    if (!sections.isEmpty()) return sections.join("\n\n");
    // Tiny requested budgets cannot hold even one complete sentence.
    return InsufficientSummary.left(budget);
}
}

QJsonObject AssistantManager::presentResearchResult(const QString &name, const QJsonObject &args,
                                                     const QString &, const QJsonObject &source) const
{
    if (!researchTool(name) || source.value("summary_version").toInt() == 1) return source;
    auto result = source;
    bool summarized = false;
    const QString focus = args.value("focus").toString(args.value("query").toString())
        + ' ' + source.value("title").toString();
    const auto summarize = [&](QJsonObject &object, const QString &field, int budget) {
        if (!object.value(field).isString()) return;
        const auto content = object.value(field).toString();
        if (content.size() <= budget) return;
        object.insert(field, researchSummary(content, focus, budget));
        object.insert(field + "_summarized", true);
        summarized = true;
    };
    const auto maxChars = args.value("max_chars").toInteger(8000);
    const auto snippetChars = args.value("snippet_chars").toInteger(400);
    const auto linkOffset = args.value("link_offset").toInteger(0);
    const auto maxLinks = args.value("max_links").toInteger(10);
    const auto limit = args.value("limit").toInteger(10);
    if (maxChars < 0 || snippetChars < 0 || linkOffset < 0 || maxLinks < 0 || limit < 0)
        return {{"error", "Research sizes and offsets must be nonnegative."}};
    if (source.value("content").isString()) {
        result.insert("total_chars", source.value("total_chars").toInteger(source.value("content").toString().size()));
        summarize(result, "content", qMin<qint64>(8000, maxChars));
    }
    if (source.value("links").isArray()) {
        const auto links = source.value("links").toArray();
        const auto start = qMin<qint64>(linkOffset, links.size());
        const auto count = qMin<qint64>(maxLinks, links.size() - start);
        QJsonArray shown;
        for (qint64 index = start; index < start + count; ++index) {
            auto link = links[index].toObject();
            link.insert("title", link.value("title").toString().left(160));
            shown.append(link);
        }
        result.insert("links", shown);
        result.insert("link_offset", start);
        result.insert("total_links", source.value("total_links").toInteger(links.size()));
        result.insert("next_link_offset", start + count < links.size() ? QJsonValue(start + count) : QJsonValue());
    }
    if (source.value("results").isArray()) {
        const auto items = source.value("results").toArray();
        QJsonArray shown;
        for (qsizetype index = 0; index < qMin<qint64>(limit, items.size()); ++index) {
            auto item = items[index].toObject();
            const QStringList fields{"snippet", "abstract", "body", "description"};
            for (const auto &field : fields)
                summarize(item, field, field == "snippet" ? qMin<qint64>(400, snippetChars) : 1600);
            item.insert("title", item.value("title").toString().left(200));
            item.remove("snippet_truncated");
            shown.append(item);
        }
        result.insert("results", shown);
        result.insert("total_results", source.value("total_results").toInteger(items.size()));
    }
    for (const auto &field : {"excerpted", "truncated", "offset", "next_offset", "full_result_path", "full_text_path"})
        result.remove(field);
    result.insert("summary_version", 1);
    result.insert("summarized", summarized);
    result.insert("original_retained", false);
    if (summarized || source.contains("full_result_path") || source.value("excerpted").toBool())
        result.insert("note", "Large content is summarized using selected source sentences for main points and key figures, "
            "not every fact. Original content is not retained. Re-fetch the source with focus for missing details; "
            "keep units, dates, qualifiers and uncertainty when citing numbers.");
    return result;
}

QJsonArray AssistantManager::researchRequestHistory() const
{
    auto history = m_history;
    QHash<QString, QJsonObject> calls;
    for (qsizetype index = 0; index < history.size(); ++index) {
        auto message = history[index].toObject();
        for (const auto &value : message.value("tool_calls").toArray()) {
            const auto call = value.toObject();
            calls.insert(call.value("id").toString(), researchFunction(call));
        }
        if (message.value("role") != "tool") continue;
        const auto id = message.value("tool_call_id").toString();
        const auto call = calls.value(id);
        const auto name = call.value("name").toString();
        if (!researchTool(name)) continue;
        const auto document = QJsonDocument::fromJson(message.value("content").toString().toUtf8());
        if (!document.isObject()) continue;
        const auto args = QJsonDocument::fromJson(call.value("arguments").toString().toUtf8()).object();
        message.insert("content", QString::fromUtf8(QJsonDocument(presentResearchResult(name, args, id, document.object()))
            .toJson(QJsonDocument::Compact)));
        history[index] = message;
    }
    return history;
}

void AssistantManager::migrateResearchChats()
{
    // Replace only research tool evidence, in both model history and its UI row.
    // Commit the summary before removing archives created by the old presenter.
    const QDir root(chatRoot());
    for (const auto &directory : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        const QString path = directory.filePath() + "/chat.json";
        if (QFileInfo(path).isSymLink()) continue;
        QFile input(path);
        if (!input.open(QIODevice::ReadOnly)) continue;
        auto data = QJsonDocument::fromJson(input.readAll()).object();
        input.close();
        if (data.value("version").toInt() != 1) continue;
        auto history = data.value("history").toArray();
        QHash<QString, QJsonObject> calls, results;
        QHash<QString, QByteArray> archives;
        QSet<QString> retained;
        const QString archiveDirectory = workspaceRoot() + "/research/" + directory.fileName();
        const auto validArchive = [&](const QString &archive) {
            static const QRegularExpression filename("^[a-f0-9]{24}-[a-f0-9]{16}\\.(json|txt)$");
            const QFileInfo file(archive);
            return !archive.isEmpty() && file.absolutePath() == archiveDirectory
                && filename.match(file.fileName()).hasMatch() && file.isFile() && !file.isSymLink()
                && QDir(archiveDirectory).canonicalPath() == QDir::cleanPath(archiveDirectory);
        };
        const auto readArchive = [&](const QString &archive, QByteArray &bytes) {
            if (!validArchive(archive)) return false;
            QFile file(archive);
            if (!file.open(QIODevice::ReadOnly) || file.size() > MaxResearchBytes) return false;
            bytes = file.read(MaxResearchBytes + 1);
            return !bytes.isEmpty() && bytes.size() <= MaxResearchBytes && file.error() == QFile::NoError;
        };
        for (qsizetype index = 0; index < history.size(); ++index) {
            auto message = history[index].toObject();
            for (const auto &value : message.value("tool_calls").toArray()) {
                const auto call = value.toObject();
                calls.insert(call.value("id").toString(), researchFunction(call));
            }
            if (message.value("role") != "tool") continue;
            const auto id = message.value("tool_call_id").toString();
            const auto call = calls.value(id);
            if (!researchTool(call.value("name").toString())) continue;
            auto source = QJsonDocument::fromJson(message.value("content").toString().toUtf8()).object();
            if (source.isEmpty() || source.value("summary_version").toInt() == 1) continue;
            const QString originalPath = source.value("full_result_path").toString();
            const QString textPath = source.value("full_text_path").toString();
            const QStringList references{originalPath, textPath};
            auto keep = qScopeGuard([&] { for (const auto &archive : references) if (!archive.isEmpty()) retained.insert(archive); });
            QByteArray original;
            QByteArray text;
            if (!originalPath.isEmpty()) {
                if (!readArchive(originalPath, original)) continue;
                const auto full = QJsonDocument::fromJson(original).object();
                if (!full.value("content").isString() && !full.value("results").isArray()) continue;
                source = full;
            }
            if (!textPath.isEmpty()) {
                if (!readArchive(textPath, text)) continue;
                QStringDecoder decoder(QStringDecoder::Utf8);
                const QString content = decoder(text);
                if (decoder.hasError()) continue;
                if (!originalPath.isEmpty() && source.value("content").toString() != content) continue;
                source.insert("content", content);
            }
            const auto args = QJsonDocument::fromJson(call.value("arguments").toString().toUtf8());
            if (!args.isObject()) continue;
            const auto result = presentResearchResult(call.value("name").toString(), args.object(), id, source);
            const auto summary = result.value("content").toString();
            if (result.contains("error") || result.value("summary_version").toInt() != 1
                || (!source.value("content").toString().isEmpty() && (summary.isEmpty() ||
                    (result.value("content_summarized").toBool() && InsufficientSummary.startsWith(summary))))
                || (!source.value("results").toArray().isEmpty() && result.value("results").toArray().isEmpty())) continue;
            if (!originalPath.isEmpty()) archives.insert(originalPath, QCryptographicHash::hash(original, QCryptographicHash::Sha256));
            if (!textPath.isEmpty()) archives.insert(textPath, QCryptographicHash::hash(text, QCryptographicHash::Sha256));
            keep.dismiss();
            results.insert(id, result);
            message.insert("content", QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
            history[index] = message;
        }
        if (results.isEmpty()) continue;
        auto messages = data.value("messages").toArray();
        for (qsizetype index = 0; index < messages.size(); ++index) {
            auto message = messages[index].toObject();
            const auto id = message.value("callId").toString(message.value("id").toString());
            if (message.value("role") != "tool" || !results.contains(id)) continue;
            message.insert("output", QString::fromUtf8(QJsonDocument(results.value(id)).toJson(QJsonDocument::Indented)).trimmed());
            messages[index] = message;
        }
        data.insert("history", history);
        data.insert("messages", messages);
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly)) continue;
        output.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
        if (output.write(bytes) != bytes.size() || !output.commit()) continue;
        for (auto archive = archives.cbegin(); archive != archives.cend(); ++archive) {
            QByteArray bytes;
            if (!retained.contains(archive.key()) && readArchive(archive.key(), bytes)
                && archive.value() == QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)) QFile::remove(archive.key());
        }
        QDir().rmdir(archiveDirectory); // Only removes an empty directory.
    }
}

QJsonObject AssistantManager::researchResult(const QString &name, const QByteArray &data,
                                            const QUrl &url, const QString &mime)
{
    QJsonObject result{{"url", url.toString()},
        {"retrieved_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (name == "read_webpage") {
        if (!(mime.startsWith("text/") || mime == "application/json" || mime == "application/xhtml+xml")
            || data.contains('\0'))
            return {{"error", "This URL is not an HTML or text page. Use a text/abstract page for this source."}, {"url", url.toString()}};
        const QString source = QString::fromUtf8(data);
        const bool html = mime == "text/html" || mime == "application/xhtml+xml";
        const QString text = html ? plainText(source) : source;
        if (html) {
            const auto title = QRegularExpression("<title\\b[^>]*>(.*?)</title>",
                QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption).match(source);
            result.insert("title", plainText(title.captured(1)));
            QJsonArray links;
            QSet<QString> seen;
            auto matches = QRegularExpression("<a\\b[^>]*href\\s*=\\s*([\"'])(.*?)\\1[^>]*>(.*?)</a>",
                QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption).globalMatch(source);
            while (matches.hasNext()) {
                const auto match = matches.next();
                const QUrl link = url.resolved(QUrl(plainText(match.captured(2))));
                if (!webUrl(link) || seen.contains(link.toString())) continue;
                seen.insert(link.toString());
                links.append(QJsonObject{{"title", plainText(match.captured(3))}, {"url", link.toString()}});
            }
            result.insert("links", links);
        }
        result.insert("content", text);
        result.insert("truncated", false);
        return result;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return {{"error", "The source returned invalid JSON."}, {"url", url.toString()}};
    const auto object = document.object();
    QJsonArray items;
    if (name == "search_wikipedia") {
        if (!object.value("query").isObject())
            return {{"error", "Wikipedia did not return search results."}, {"url", url.toString()}};
        for (const auto &value : object.value("query").toObject().value("search").toArray()) {
            const auto item = value.toObject();
            items.append(QJsonObject{{"title", item.value("title")},
                {"snippet", plainText(item.value("snippet").toString())},
                {"url", "https://en.wikipedia.org/?curid=" + QString::number(item.value("pageid").toInteger())}});
        }
    } else if (name == "search_papers") {
        if (!object.value("message").toObject().value("items").isArray())
            return {{"error", "Crossref did not return search results."}, {"url", url.toString()}};
        for (const auto &value : object.value("message").toObject().value("items").toArray()) {
            const auto item = value.toObject();
            QJsonArray authors;
            for (const auto &author : item.value("author").toArray()) {
                const auto person = author.toObject();
                authors.append((person.value("given").toString() + ' ' + person.value("family").toString()).trimmed());
            }
            const auto titles = item.value("title").toArray();
            items.append(QJsonObject{{"title", titles.isEmpty() ? QJsonValue("") : titles.first()},
                {"url", item.value("URL")}, {"doi", item.value("DOI")}, {"authors", authors},
                {"published", item.value("published")},
                {"abstract", plainText(item.value("abstract").toString())}});
        }
        result.insert("note", "Crossref provides publication metadata and some abstracts, not full papers. Read primary sources before citing their findings.");
    }
    result.insert("results", items);
    return result;
}

bool AssistantManager::runResearchTool(const QString &name, const QJsonObject &args)
{
    if (name != "search_wikipedia" && name != "search_papers" && name != "read_webpage")
        return false;
    QUrl url;
    if (name == "read_webpage") {
        url = QUrl(args.value("url").toString());
        if (!webUrl(url) || url.toEncoded().size() > 4096) {
            finishTool({{"error", "Provide an HTTP or HTTPS URL without embedded credentials."}});
            return true;
        }
    } else {
        const QString query = args.value("query").toString().trimmed();
        if (query.isEmpty() || query.size() > 1024) {
            finishTool({{"error", "Provide a search query of 1 to 1,024 characters."}});
            return true;
        }
        const QString limit = QString::number(args.value("limit").toInteger(10));
        QUrlQuery parameters;
        if (name == "search_wikipedia") {
            url = QUrl("https://en.wikipedia.org/w/api.php");
            parameters = QUrlQuery{{"action", "query"}, {"list", "search"}, {"srsearch", query},
                {"srlimit", limit}, {"sroffset", QString::number(args.value("offset").toInteger(0))}, {"format", "json"}};
        } else {
            url = QUrl("https://api.crossref.org/works");
            parameters = QUrlQuery{{"query", query}, {"rows", limit}, {"offset", QString::number(args.value("offset").toInteger(0))}};
        }
        url.setQuery(parameters);
    }
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "Pilon/1.0 (research; QtNetwork)");
    request.setRawHeader("Accept", name == "read_webpage" ? "text/html, text/plain, application/json" : "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setMaximumRedirectsAllowed(5);
    request.setTransferTimeout(10000);
    auto *reply = m_network.get(request);
    m_researchReply = reply;
    m_toolExecuting = true;
    reply->setReadBufferSize(64 * 1024);
    struct Transfer { QByteArray body; QString failure; };
    const auto transfer = std::make_shared<Transfer>();
    QTimer::singleShot(20000, reply, [reply, transfer] {
        if (!reply->isRunning()) return;
        transfer->failure = QStringLiteral("The source did not respond within 20 seconds. Try again or use a different source.");
        reply->abort();
    });
    connect(reply, &QNetworkReply::redirected, this, [reply, transfer, from = url](const QUrl &destination) mutable {
        const auto resolved = from.resolved(destination);
        if (webUrl(resolved) && resolved.toEncoded().size() <= 4096
            && !(from.scheme() == "https" && resolved.scheme() == "http")) {
            from = resolved;
            transfer->body.clear();
            reply->redirectAllowed();
        } else {
            transfer->failure = QStringLiteral("The source redirected to an unsupported URL.");
            reply->abort();
        }
    });
    const auto consume = [reply, transfer] {
        if (!transfer->failure.isEmpty()) return;
        const auto remaining = MaxResearchBytes - transfer->body.size();
        if (reply->bytesAvailable() > remaining) {
            transfer->failure = QStringLiteral("The source response exceeds 8 MiB. Use a smaller source or a direct section link.");
            if (reply->isRunning()) reply->abort();
            return;
        }
        transfer->body.append(reply->readAll());
    };
    connect(reply, &QNetworkReply::readyRead, this, consume);
    connect(reply, &QNetworkReply::finished, this, [this, reply, transfer, name, consume] {
        if (m_researchReply != reply) { reply->deleteLater(); return; }
        consume();
        m_researchReply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonObject result;
        if (!transfer->failure.isEmpty()) {
            result = {{"error", transfer->failure}, {"http_status", status}};
        } else if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
            result = {{"error", reply->errorString()}, {"http_status", status},
                {"retry_after", QString::fromLatin1(reply->rawHeader("Retry-After"))}};
        else result = researchResult(name, transfer->body, reply->url(),
            reply->header(QNetworkRequest::ContentTypeHeader).toString().section(';', 0, 0).trimmed().toLower());
        result.insert("url", reply->url().toString());
        reply->deleteLater();
        if (m_busy && !m_currentTool.isEmpty()) finishTool(result);
    });
    return true;
}
