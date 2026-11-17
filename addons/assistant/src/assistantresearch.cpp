#include "assistantmanager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextDocumentFragment>
#include <QUrlQuery>
#include <limits>
#include <cmath>

namespace {
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
        || name == "search_wikipedia" || name == "search_papers" || name == "search_github";
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
    for (const QString &part : content.split(boundaries, Qt::SkipEmptyParts)) {
        const auto line = part.simplified();
        qsizetype position = 0;
        // Bound exceptionally long sentences and minified text, keeping word
        // boundaries where possible and marking incomplete source sentences.
        while (position < line.size()) {
            const int maximum = qBound(64, (budget - 80) / 2, 600);
            int length = qMin<qsizetype>(line.size() - position, maximum);
            if (position + length < line.size()) {
                const auto space = line.lastIndexOf(' ', position + length);
                if (space > position + maximum / 2) length = space - position;
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
    return QStringLiteral("Summary budget too small; request more characters.").left(budget);
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
        QStringList archives;
        const QString archiveDirectory = workspaceRoot() + "/research/" + directory.fileName();
        const auto validArchive = [&](const QString &archive) {
            static const QRegularExpression filename("^[a-f0-9]{24}-[a-f0-9]{16}\\.(json|txt)$");
            const QFileInfo file(archive);
            return !archive.isEmpty() && file.absolutePath() == archiveDirectory
                && filename.match(file.fileName()).hasMatch() && !file.isSymLink()
                && QDir(archiveDirectory).canonicalPath() == QDir::cleanPath(archiveDirectory);
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
            for (const auto &key : {"full_result_path", "full_text_path"}) {
                const auto archive = source.value(key).toString();
                if (validArchive(archive)) archives.append(archive);
            }
            const QString originalPath = source.value("full_result_path").toString();
            if (validArchive(originalPath)) {
                QFile original(originalPath);
                if (original.open(QIODevice::ReadOnly)) {
                    const auto full = QJsonDocument::fromJson(original.readAll()).object();
                    if (!full.isEmpty()) source = full;
                }
            }
            const auto args = QJsonDocument::fromJson(call.value("arguments").toString().toUtf8()).object();
            const auto result = presentResearchResult(call.value("name").toString(), args, id, source);
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
        for (const auto &archive : archives) QFile::remove(archive);
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
    } else {
        if (!object.value("items").isArray())
            return {{"error", "GitHub did not return search results."}, {"url", url.toString()}};
        for (const auto &value : object.value("items").toArray()) {
            const auto item = value.toObject();
            items.append(QJsonObject{{"title", item.contains("full_name") ? item.value("full_name") : item.value("title")},
                {"url", item.value("html_url")}, {"description", item.value("description").toString()},
                {"body", item.value("body").toString()}, {"updated_at", item.value("updated_at")}});
        }
    }
    result.insert("results", items);
    return result;
}

bool AssistantManager::runResearchTool(const QString &name, const QJsonObject &args)
{
    if (name != "search_wikipedia" && name != "search_papers" && name != "search_github" && name != "read_webpage")
        return false;
    QUrl url;
    if (name == "read_webpage") {
        url = QUrl(args.value("url").toString());
        if (!webUrl(url)) {
            finishTool({{"error", "Provide an HTTP or HTTPS URL without embedded credentials."}});
            return true;
        }
    } else {
        const QString query = args.value("query").toString().trimmed();
        if (query.isEmpty()) {
            finishTool({{"error", "Provide a search query."}});
            return true;
        }
        const QString limit = QString::number(args.value("limit").toInteger(10));
        QUrlQuery parameters;
        if (name == "search_wikipedia") {
            url = QUrl("https://en.wikipedia.org/w/api.php");
            parameters = QUrlQuery{{"action", "query"}, {"list", "search"}, {"srsearch", query},
                {"srlimit", limit}, {"sroffset", QString::number(args.value("offset").toInteger(0))}, {"format", "json"}};
        } else if (name == "search_papers") {
            url = QUrl("https://api.crossref.org/works");
            parameters = QUrlQuery{{"query", query}, {"rows", limit}, {"offset", QString::number(args.value("offset").toInteger(0))}};
        } else {
            const QString kind = args.value("kind").toString("repositories");
            if (kind != "repositories" && kind != "issues") {
                finishTool({{"error", "kind must be repositories or issues."}});
                return true;
            }
            url = QUrl("https://api.github.com/search/" + kind);
            parameters = QUrlQuery{{"q", query}, {"per_page", limit}, {"page", QString::number(args.value("page").toInteger(1))}};
        }
        url.setQuery(parameters);
    }
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "Pilon/1.0 (research; QtNetwork)");
    request.setRawHeader("Accept", name == "read_webpage" ? "text/html, text/plain, application/json" : "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setMaximumRedirectsAllowed(std::numeric_limits<int>::max());
    request.setTransferTimeout(0);
    auto *reply = m_network.get(request);
    m_researchReply = reply;
    m_toolExecuting = true;
    const auto body = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::redirected, this, [reply](const QUrl &destination) {
        if (webUrl(destination) && !(reply->url().scheme() == "https" && destination.scheme() == "http"))
            reply->redirectAllowed();
        else reply->abort();
    });
    const auto consume = [reply, body] { body->append(reply->readAll()); };
    connect(reply, &QNetworkReply::readyRead, this, consume);
    connect(reply, &QNetworkReply::finished, this, [this, reply, body, name, consume] {
        if (m_researchReply != reply) { reply->deleteLater(); return; }
        consume();
        m_researchReply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonObject result;
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300)
            result = {{"error", reply->errorString()}, {"http_status", status},
                {"retry_after", QString::fromLatin1(reply->rawHeader("Retry-After"))}};
        else result = researchResult(name, *body, reply->url(),
            reply->header(QNetworkRequest::ContentTypeHeader).toString().section(';', 0, 0).trimmed().toLower());
        result.insert("url", reply->url().toString());
        reply->deleteLater();
        if (m_busy && !m_currentTool.isEmpty()) finishTool(result);
    });
    return true;
}
