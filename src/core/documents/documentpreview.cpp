#include "documentpreview.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtConcurrent>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#ifdef Q_OS_ANDROID
#include <QImage>
#include <QMutex>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <fpdf_doc.h>
#include <fpdf_text.h>
#include <fpdf_formfill.h>
#include <fpdf_progressive.h>
#include <cpp/fpdf_scopers.h>
#include <mutex>
#endif

struct DocumentPreview::State {
    QString source;
    QTemporaryDir directory;
    std::atomic_bool cancelled = false;
};
namespace {
QRectF rectangle(const QVariant &value) {
    const auto coords = value.toList();
    return coords.size() == 4 ? QRectF(coords[0].toDouble(), coords[1].toDouble(), coords[2].toDouble(), coords[3].toDouble()) : QRectF{};
}
#ifdef Q_OS_ANDROID
struct PdfLibrary {
    QMutex mutex;
    PdfLibrary() { FPDF_InitLibrary(); }
    ~PdfLibrary() { FPDF_DestroyLibrary(); }
};
QSizeF pdfSize(FPDF_PAGE page) {
    const double width = page ? FPDF_GetPageWidthF(page) : 0, height = page ? FPDF_GetPageHeightF(page) : 0;
    return std::isfinite(width) && std::isfinite(height) && width > 0 && height > 0 && width <= 1000000 && height <= 1000000
        ? QSizeF(width, height) : QSizeF{};
}
QPointF pdfPoint(FPDF_PAGE page, double x, double y) {
    const auto size = pdfSize(page);
    if (size.isEmpty() || !std::isfinite(x) || !std::isfinite(y)) return {};
    int dx = 0, dy = 0;
    FPDF_PageToDevice(page, 0, 0, qRound(size.width() * 1024), qRound(size.height() * 1024), 0, x, y, &dx, &dy);
    return {dx / 1024.0, dy / 1024.0};
}
QRectF pdfRectangle(FPDF_PAGE page, double left, double bottom, double right, double top) {
    return QRectF(pdfPoint(page, left, bottom), pdfPoint(page, right, top)).normalized();
}
QVariantList pdfBox(const QRectF &box) { return {box.x(), box.y(), box.width(), box.height()}; }
QVariantMap pdfText(FPDF_PAGE page, const std::atomic_bool *cancel) {
    if (pdfSize(page).isEmpty()) return {{"error", QObject::tr("Invalid PDF page dimensions.")}};
    ScopedFPDFTextPage textPage(FPDFText_LoadPage(page));
    if (!textPage) return {{"error", QObject::tr("Unable to read PDF text.")}};
    const int count = FPDFText_CountChars(textPage.get());
    QString text;
    QVariantList boxes;
    for (int i = 0; i < count; ++i) {
        if (cancel->load()) return {{"error", QObject::tr("Document decoding cancelled.")}};
        const char32_t codepoint = FPDFText_GetUnicode(textPage.get(), i);
        if (!codepoint || codepoint == '\r') continue;
        const auto value = QString::fromUcs4(&codepoint, 1);
        QRectF box;
        double left = 0, right = 0, bottom = 0, top = 0;
        if (codepoint != '\n' && FPDFText_GetCharBox(textPage.get(), i, &left, &right, &bottom, &top))
            box = pdfRectangle(page, left, bottom, right, top);
        text += value;
        for (int unit = 0; unit < value.size(); ++unit) boxes.append(QVariant(pdfBox(box)));
    }
    return {{"text", text}, {"characters", boxes}};
}
QVariantMap pdfDestination(FPDF_DOCUMENT document, FPDF_DEST destination) {
    const int number = destination ? FPDFDest_GetDestPageIndex(document, destination) : -1;
    if (number < 0 || number >= FPDF_GetPageCount(document)) return {{"page", -1}, {"top", 0.0}};
    ScopedFPDFPage page(FPDF_LoadPage(document, number));
    const auto size = pdfSize(page.get());
    if (size.isEmpty()) return {{"page", -1}, {"top", 0.0}};
    double x = 0, y = 0;
    FPDF_DeviceToPage(page.get(), 0, 0, qRound(size.width() * 1024), qRound(size.height() * 1024), 0, 0, 0, &x, &y);
    FPDF_BOOL hasX = false, hasY = false, hasZoom = false;
    float targetX = 0, targetY = 0, zoom = 0;
    if (FPDFDest_GetLocationInPage(destination, &hasX, &hasY, &hasZoom, &targetX, &targetY, &zoom)) {
        if (hasX) x = targetX;
        if (hasY) y = targetY;
    } else {
        unsigned long parameters = 0;
        float values[4]{};
        const auto view = FPDFDest_GetView(destination, &parameters, values);
        if ((view == PDFDEST_VIEW_FITH || view == PDFDEST_VIEW_FITBH) && parameters) y = values[0];
        if ((view == PDFDEST_VIEW_FITV || view == PDFDEST_VIEW_FITBV) && parameters) x = values[0];
        if (view == PDFDEST_VIEW_FITR && parameters == 4) { x = values[0]; y = values[3]; }
    }
    const double top = pdfPoint(page.get(), x, y).y() / size.height();
    return {{"page", number}, {"top", top}};
}
FPDF_DEST pdfActionDestination(FPDF_DOCUMENT document, FPDF_DEST direct, FPDF_ACTION action) {
    return direct ? direct : action && FPDFAction_GetType(action) == PDFACTION_GOTO ? FPDFAction_GetDest(document, action) : nullptr;
}
QVariantList pdfSections(FPDF_DOCUMENT document, FPDF_BOOKMARK parent, QSet<FPDF_BOOKMARK> &visited,
    int depth, const std::atomic_bool *cancel, QString &error) {
    QVariantList sections;
    for (auto bookmark = FPDFBookmark_GetFirstChild(document, parent); bookmark; bookmark = FPDFBookmark_GetNextSibling(document, bookmark)) {
        if (cancel->load()) { error = QObject::tr("Document decoding cancelled."); break; }
        if (depth > 64 || visited.size() >= 100000 || visited.contains(bookmark)) { error = QObject::tr("Invalid PDF bookmarks."); break; }
        visited.insert(bookmark);
        const auto length = FPDFBookmark_GetTitle(bookmark, nullptr, 0);
        if (length > 1048576) { error = QObject::tr("Invalid PDF bookmark title."); break; }
        QList<char16_t> title((length + 1) / 2);
        if (length) FPDFBookmark_GetTitle(bookmark, title.data(), length);
        auto section = pdfDestination(document, pdfActionDestination(document, FPDFBookmark_GetDest(document, bookmark), FPDFBookmark_GetAction(bookmark)));
        section["title"] = length >= 2 ? QString::fromUtf16(title.constData(), length / 2 - 1) : QString{};
        section["children"] = pdfSections(document, bookmark, visited, depth + 1, cancel, error);
        if (!error.isEmpty()) break;
        sections.append(section);
    }
    return sections;
}
QVariantMap requestDocument(const QVariantMap &request, const std::atomic_bool *cancel) {
    // PDFium is not thread-safe; rendering and search use separate Qt workers.
    static PdfLibrary library;
    while (!library.mutex.tryLock(25))
        if (cancel->load()) return {{"error", QObject::tr("Document decoding cancelled.")}};
    std::unique_lock lock(library.mutex, std::adopt_lock);
    if (cancel->load()) return {{"error", QObject::tr("Document decoding cancelled.")}};
    const auto source = QFile::encodeName(request.value("source").toString());
    ScopedFPDFDocument document(FPDF_LoadDocument(source.constData(), nullptr));
    if (!document) return {{"error", FPDF_GetLastError() == FPDF_ERR_PASSWORD ? QObject::tr("This PDF requires a password.") : QObject::tr("Unable to open this PDF.")}};
    const int count = FPDF_GetPageCount(document.get());
    if (count <= 0) return {{"error", QObject::tr("This PDF has no pages.")}};
    const auto command = request.value("command").toString();
    if (command == "open") {
        QVariantList pages;
        for (int i = 0; i < count; ++i) {
            if (cancel->load()) return {{"error", QObject::tr("Document decoding cancelled.")}};
            ScopedFPDFPage page(FPDF_LoadPage(document.get(), i));
            const auto size = pdfSize(page.get());
            if (size.isEmpty()) return {{"error", QObject::tr("Invalid PDF page dimensions.")}};
            pages.append(QVariantMap{{"width", size.width()}, {"height", size.height()}});
        }
        QSet<FPDF_BOOKMARK> visited;
        QString error;
        const auto sections = pdfSections(document.get(), nullptr, visited, 0, cancel, error);
        return error.isEmpty() ? QVariantMap{{"pages", pages}, {"sections", sections}} : QVariantMap{{"error", error}};
    }
    if (command == "search") {
        QVariantList pages;
        const bool all = request.value("all").toBool(), backwards = request.value("backwards").toBool();
        const QRegularExpression pattern(QRegularExpression::escape(request.value("text").toString()), QRegularExpression::CaseInsensitiveOption);
        if (!all && request.value("text").toString().isEmpty()) return {{"page", -1}};
        for (int offset = 0; offset < (all ? count : count + 1); ++offset) {
            if (cancel->load()) return {{"error", QObject::tr("Document decoding cancelled.")}};
            const auto from = qint64(request.value("from").toInt()) + (backwards ? -offset : offset);
            const int number = all ? offset : int((from % count + count) % count);
            ScopedFPDFPage page(FPDF_LoadPage(document.get(), number));
            if (!page) return {{"error", QObject::tr("Unable to load this PDF page.")}};
            const auto text = pdfText(page.get(), cancel);
            if (text.contains("error")) return text;
            if (all) { pages.append(text); continue; }
            const auto content = text.value("text").toString();
            QList<QRegularExpressionMatch> matches;
            auto iterator = pattern.globalMatch(content);
            while (iterator.hasNext()) matches.append(iterator.next());
            if (backwards) std::reverse(matches.begin(), matches.end());
            for (const auto &match : matches) {
                const int start = match.capturedStart();
                if (!offset && request.value("continuation").toBool()
                    && (backwards ? start >= request.value("anchor").toInt() : start < request.value("cursor").toInt())) continue;
                return {{"page", number}, {"offset", start}, {"length", match.capturedLength()}, {"pages", QVariantList{text}}};
            }
        }
        return all ? QVariantMap{{"pages", pages}, {"all", true}} : QVariantMap{{"page", -1}};
    }
    if (command == "render") {
        const int number = request.value("page").toInt();
        if (number < 0 || number >= count) return {{"error", QObject::tr("Invalid PDF page number.")}};
        ScopedFPDFPage page(FPDF_LoadPage(document.get(), number));
        if (!page) return {{"error", QObject::tr("Unable to load this PDF page.")}};
        const auto size = pdfSize(page.get());
        if (size.isEmpty()) return {{"error", QObject::tr("Invalid PDF page dimensions.")}};
        const double width = size.width(), height = size.height();
        const double scale = std::clamp(request.value("resolution").toInt(), 1, 3600) / std::max(width, height);
        QImage image(QSize(std::max(1, int(std::ceil(width * scale))), std::max(1, int(std::ceil(height * scale)))), QImage::Format_ARGB32);
        if (image.isNull()) return {{"error", QObject::tr("Unable to allocate the PDF page image.")}};
        image.fill(Qt::white);
        ScopedFPDFBitmap bitmap(FPDFBitmap_CreateEx(image.width(), image.height(), FPDFBitmap_BGRA, image.bits(), image.bytesPerLine()));
        if (!bitmap) return {{"error", QObject::tr("Unable to render this PDF page.")}};
        struct FormInfo : FPDF_FORMFILLINFO { FPDF_PAGE page; int number; } formInfo{};
        formInfo.version = 2; formInfo.page = page.get(); formInfo.number = number;
        formInfo.FFI_GetPage = [](FPDF_FORMFILLINFO *info, FPDF_DOCUMENT, int index) -> FPDF_PAGE {
            const auto form = static_cast<FormInfo *>(info);
            return index == form->number ? form->page : nullptr;
        };
        formInfo.FFI_GetRotation = [](FPDF_FORMFILLINFO *, FPDF_PAGE) { return 0; };
        ScopedFPDFFormHandle form(FPDFDOC_InitFormFillEnvironment(document.get(), &formInfo));
        if (form) FORM_OnAfterLoadPage(page.get(), form.get());
        struct PauseState { const std::atomic_bool *cancel; QElapsedTimer timer, slice; } pauseState{cancel, {}, {}};
        pauseState.timer.start(); pauseState.slice.start();
        IFSDK_PAUSE pause{1, [](IFSDK_PAUSE *info) -> FPDF_BOOL {
            const auto state = static_cast<PauseState *>(info->user);
            return state->cancel->load() || state->timer.elapsed() > 120000 || state->slice.elapsed() >= 8;
        }, &pauseState};
        int status = FPDF_RenderPageBitmap_Start(bitmap.get(), page.get(), 0, 0, image.width(), image.height(), 0, FPDF_ANNOT, &pause);
        while (status == FPDF_RENDER_TOBECONTINUED && !cancel->load() && pauseState.timer.elapsed() <= 120000) {
            // A search request can use PDFium between short render slices.
            lock.unlock(); QThread::msleep(1); lock.lock();
            if (cancel->load() || pauseState.timer.elapsed() > 120000) break;
            pauseState.slice.restart();
            status = FPDF_RenderPage_Continue(page.get(), &pause);
        }
        FPDF_RenderPage_Close(page.get());
        if (form) {
            if (status == FPDF_RENDER_DONE && !cancel->load())
                FPDF_FFLDraw(form.get(), bitmap.get(), page.get(), 0, 0, image.width(), image.height(), 0, FPDF_ANNOT);
            FORM_OnBeforeClosePage(page.get(), form.get());
        }
        if (cancel->load() || pauseState.timer.elapsed() > 120000) return {{"error", QObject::tr("Document decoding cancelled or timed out.")}};
        if (status != FPDF_RENDER_DONE) return {{"error", QObject::tr("Unable to render this PDF page.")}};
        auto result = pdfText(page.get(), cancel);
        if (result.contains("error")) return result;
        QVariantList links;
        int position = 0;
        FPDF_LINK link = nullptr;
        while (FPDFLink_Enumerate(page.get(), &position, &link)) {
            if (cancel->load()) return {{"error", QObject::tr("Document decoding cancelled.")}};
            auto target = pdfDestination(document.get(), pdfActionDestination(document.get(), FPDFLink_GetDest(document.get(), link), FPDFLink_GetAction(link)));
            FS_RECTF rectangle;
            if (target.value("page").toInt() < 0 || !FPDFLink_GetAnnotRect(link, &rectangle)) continue;
            const auto box = pdfRectangle(page.get(), rectangle.left, rectangle.bottom, rectangle.right, rectangle.top);
            target["area"] = pdfBox({box.x() / width, box.y() / height, box.width() / width, box.height() / height});
            links.append(target);
        }
        result["links"] = links;
        form.reset(); bitmap.reset(); page.reset(); document.reset();
        lock.unlock();
        QSaveFile output(request.value("output").toString());
        if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG"))
            return {{"error", QObject::tr("Unable to save the PDF page image.")}};
        if (cancel->load()) return {{"error", QObject::tr("Document decoding cancelled.")}};
        if (!output.commit())
            return {{"error", QObject::tr("Unable to save the PDF page image.")}};
        return result;
    }
    return {{"error", QObject::tr("Unknown document operation.")}};
}
#else
QVariantMap requestDocument(const QVariantMap &request, const std::atomic_bool *cancel) {
    const auto python = QDir(QCoreApplication::applicationDirPath()).filePath("../runtime/documents/bin/python");
    if (!QFileInfo::exists(python)) return {{"error", QObject::tr("The document decoder is unavailable. Run src/scripts/setup_documents.sh.")}};
    QFile script(":/qt/qml/Pilon/document_worker.py");
    if (!script.open(QIODevice::ReadOnly)) return {{"error", QObject::tr("The document decoder script is unavailable.")}};
    QProcess process;
    process.start(python, {"-c", QString::fromUtf8(script.readAll())});
    if (!process.waitForStarted()) return {{"error", process.errorString()}};
    process.write(QJsonDocument::fromVariant(request).toJson(QJsonDocument::Compact));
    process.closeWriteChannel();
    QElapsedTimer timer; timer.start();
    while (!process.waitForFinished(25)) {
        if (cancel->load() || timer.elapsed() > 120000) {
            process.kill(); process.waitForFinished();
            return {{"error", QObject::tr("Document decoding cancelled or timed out.")}};
        }
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {{"error", QObject::tr("Unable to decode this document: %1").arg(QString::fromUtf8(process.readAllStandardError()).trimmed())}};
    QJsonParseError error;
    const auto result = QJsonDocument::fromJson(process.readAllStandardOutput(), &error);
    if (error.error != QJsonParseError::NoError || !result.isObject()) return {{"error", QObject::tr("Invalid response from the document decoder.")}};
    return result.toVariant().toMap();
}
#endif
void appendSections(QStandardItem *parent, const QVariantList &sections) {
    for (const auto &value : sections) {
        const auto section = value.toMap();
        auto *item = new QStandardItem(section.value("title").toString());
        item->setData(section.value("page"), Qt::UserRole);
        item->setData(section.value("top"), Qt::UserRole + 1);
        parent->appendRow(item);
        appendSections(item, section.value("children").toList());
    }
}
}
QVariantMap DocumentPreview::decodeDocument(const QVariantMap &request, const std::atomic_bool *cancel) {
    return requestDocument(request, cancel);
}
DocumentPreview::Page DocumentPreview::decodedPage(const QVariantMap &map) {
    Page page; page.text = map.value("text").toString(); page.error = map.value("error").toString();
    for (const auto &value : map.value("characters").toList()) page.characters.append(rectangle(value));
    for (const auto &value : map.value("links").toList()) {
        auto link = value.toMap(); link["area"] = rectangle(link.value("area")); page.links.append(link);
    }
    return page;
}
DocumentPreview::DocumentPreview(QObject *parent) : QAbstractListModel(parent) {
    connect(&m_searchWorker, &QFutureWatcher<Result>::finished, this, [this] {
        m_searching = false; emit searchChanged();
        const auto result = m_searchWorker.result();
        if (result.generation != m_generation || (m_searchCancel && m_searchCancel->load())) return;
        if (result.page >= m_pages.size() || (result.length == -1 && result.pages.size() != m_pages.size())) return;
        if (result.page >= 0) {
            auto &page = m_pages[result.page];
            page.text = result.pages.first().text; page.characters = result.pages.first().characters;
            m_anchor = {result.page, result.offset}; m_cursor = {result.page, result.offset + result.length};
        } else if (result.length == -1) {
            for (int i = 0; i < result.pages.size(); ++i) {
                m_pages[i].text = result.pages[i].text; m_pages[i].characters = result.pages[i].characters;
            }
            m_anchor = {0, 0}; m_cursor = {m_pages.size() - 1, std::numeric_limits<int>::max()};
        }
        notifySelection(); if (result.length != -1) emit found(result.page);
    });
    connect(&m_worker, &QFutureWatcher<Result>::finished, this, [this] {
        m_working = false;
        const auto result = m_worker.result();
        if (result.generation != m_generation) { schedule(); return; }
        if (result.page < 0) {
            beginResetModel(); m_state = result.state; m_pages = result.pages; m_sections = result.sections;
            m_sectionModel.clear(); appendSections(m_sectionModel.invisibleRootItem(), m_sections);
            m_width = m_height = 0;
            for (auto &page : m_pages) { page.top = m_height; m_height += page.size.height(); m_width = std::max(m_width, page.size.width()); }
            m_error = result.error; m_loading = false;
            endResetModel(); emit changed();
        } else {
            auto page = result.pages.first(); page.top = m_pages[result.page].top;
            m_pages[result.page] = page;
            emit dataChanged(index(result.page), index(result.page));
            emit pageRendered(result.page);
            emit selectionChanged();
        }
        schedule();
    });
}
DocumentPreview::~DocumentPreview() {
    if (m_state) m_state->cancelled = true;
    if (m_searchCancel) *m_searchCancel = true;
    m_worker.waitForFinished(); m_searchWorker.waitForFinished();
}
int DocumentPreview::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_pages.size(); }
QHash<int, QByteArray> DocumentPreview::roleNames() const {
    return {{Width, "pageWidth"}, {Height, "pageHeight"}, {Top, "pageTop"}, {Image, "pageImage"}, {Highlights, "highlights"}, {PageError, "pageError"}};
}
QVariant DocumentPreview::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_pages.size()) return {};
    const auto &page = m_pages[index.row()];
    switch (role) {
    case Width: return page.size.width();
    case Height: return page.size.height();
    case Top: return page.top;
    case Image: return page.image;
    case PageError: return page.error;
    case Highlights: {
        QVariantList boxes; const auto [start, end] = range(index.row());
        for (int i = start; i < end && i < page.characters.size(); ++i) {
            const auto box = page.characters[i];
            if (!box.isEmpty()) boxes.append(box);
        }
        return boxes;
    }
    default: return {};
    }
}
void DocumentPreview::setSource(const QUrl &source) {
    if (source == m_source) return;
    if (m_state) m_state->cancelled = true;
    if (m_searchCancel) *m_searchCancel = true;
    ++m_generation; m_source = source; m_state.reset();
    beginResetModel(); m_pages.clear(); m_sections.clear(); m_sectionModel.clear(); m_visible.clear(); m_width = m_height = 0;
    m_anchor = m_cursor = {-1, 0}; m_error.clear(); m_loading = !source.isEmpty(); endResetModel();
    emit changed(); emit selectionChanged(); schedule();
}
void DocumentPreview::schedule() {
    if (m_working || m_source.isEmpty() || !m_error.isEmpty()) return;
    const auto generation = m_generation;
    if (!m_state) {
        auto state = std::make_shared<State>(); m_state = state;
        const auto path = m_source.toLocalFile();
        m_working = true;
        m_worker.setFuture(QtConcurrent::run([state, path, generation] {
            Result result; result.state = state; result.generation = generation;
            state->source = path;
            const auto response = requestDocument({{"source", path}, {"command", "open"}}, &state->cancelled);
            result.error = response.value("error").toString();
            for (const auto &value : response.value("pages").toList()) {
                const auto page = value.toMap(); Page item;
                item.size = QSizeF(page.value("width").toDouble(), page.value("height").toDouble());
                if (item.size.isEmpty()) { result.error = tr("Invalid PDF page dimensions."); break; }
                result.pages.append(item);
            }
            result.sections = response.value("sections").toList();
            if (result.pages.isEmpty() && result.error.isEmpty()) result.error = tr("This PDF has no pages.");
            return result;
        }));
        return;
    }
    if (m_loading) return;
    int next = -1, resolution = 1500;
    for (const int i : m_visible) {
        const auto &page = m_pages[i];
        const auto wanted = std::clamp(int(std::ceil(std::max(page.size.width(), page.size.height()) * m_scale)), 1500, 3600);
        if (page.error.isEmpty() && page.resolution < wanted) { next = i; resolution = wanted; break; }
    }
    if (next < 0) for (int i = 0; i < m_pages.size(); ++i) {
        if (m_pages[i].resolution == 0 && m_pages[i].error.isEmpty()) { next = i; break; }
    }
    if (next < 0) return;
    const auto state = m_state;
    m_working = true;
    const auto size = m_pages[next].size;
    m_worker.setFuture(QtConcurrent::run([state, generation, next, resolution, size] {
        Result result; result.state = state; result.generation = generation; result.page = next;
        const auto output = state->directory.filePath(QString("%1-%2.png").arg(next).arg(resolution));
        const auto response = requestDocument({{"source", state->source}, {"command", "render"}, {"page", next},
            {"resolution", resolution}, {"output", output}}, &state->cancelled);
        auto page = decodedPage(response); page.size = size;
        if (page.error.isEmpty() && QFileInfo::exists(output)) { page.image = QUrl::fromLocalFile(output); page.resolution = resolution; }
        else if (page.error.isEmpty()) page.error = tr("Unable to render page %1.").arg(next + 1);
        result.pages.append(page); return result;
    }));
}
qreal DocumentPreview::pageY(int page, qreal scale) const {
    return page >= 0 && page < m_pages.size() ? m_pages[page].top * scale + page * 12 : 0;
}
int DocumentPreview::pageAt(qreal y, qreal scale) const {
    if (m_pages.isEmpty() || scale <= 0) return -1;
    int lo = 0, hi = m_pages.size();
    while (lo + 1 < hi) { const int mid = (lo + hi) / 2; if (pageY(mid, scale) <= y) lo = mid; else hi = mid; }
    return lo;
}
void DocumentPreview::prioritize(qreal top, qreal bottom, qreal scale) {
    m_scale = scale; m_visible.clear();
    const int first = pageAt(top, scale), last = pageAt(bottom, scale);
    for (int i = first; i >= 0 && i <= last; ++i) m_visible.append(i);
    schedule();
}
std::pair<int, int> DocumentPreview::position(qreal x, qreal y, qreal contentWidth, qreal scale) const {
    const int n = pageAt(y, scale); if (n < 0) return {-1, 0};
    const auto &page = m_pages[n];
    const QPointF point((x - (contentWidth - page.size.width() * scale) / 2) / scale, (y - pageY(n, scale)) / scale);
    qreal distance = std::numeric_limits<qreal>::max(); int offset = 0;
    for (int i = 0; i < page.characters.size(); ++i) {
        const auto &box = page.characters[i]; if (box.isEmpty()) continue;
        const qreal dx = std::max({box.left() - point.x(), point.x() - box.right(), 0.0});
        const qreal dy = std::max({box.top() - point.y(), point.y() - box.bottom(), 0.0});
        const auto d = dx * dx + dy * dy;
        if (d < distance) { distance = d; offset = i + (point.x() > box.center().x() ? 1 : 0); }
    }
    return {n, offset};
}
void DocumentPreview::select(qreal x, qreal y, qreal contentWidth, qreal scale, bool begin) {
    if (scale <= 0) return;
    m_cursor = position(x, y, contentWidth, scale); if (begin) m_anchor = m_cursor;
    notifySelection();
}
std::pair<int, int> DocumentPreview::range(int page) const {
    const auto start = std::min(m_anchor, m_cursor), end = std::max(m_anchor, m_cursor);
    if (start.first < 0 || page < start.first || page > end.first) return {0, 0};
    return {page == start.first ? start.second : 0, page == end.first ? end.second : m_pages[page].text.size()};
}
QString DocumentPreview::selectedText() const {
    QStringList pages;
    for (int i = 0; i < m_pages.size(); ++i) { const auto [start, end] = range(i); if (end > start) pages.append(m_pages[i].text.mid(start, end - start)); }
    return pages.join('\n');
}
void DocumentPreview::notifySelection() {
    if (!m_pages.isEmpty()) emit dataChanged(index(0), index(m_pages.size() - 1), {Highlights});
    emit selectionChanged();
}
void DocumentPreview::selectAll() { search({}, 0, false, true); }
void DocumentPreview::copy() const { if (!selectedText().isEmpty()) QGuiApplication::clipboard()->setText(selectedText()); }
qreal DocumentPreview::selectionY(qreal scale) const {
    const auto start = std::min(m_anchor, m_cursor);
    if (start.first < 0 || start.first >= m_pages.size()) return 0;
    const auto &page = m_pages[start.first];
    return pageY(start.first, scale) + (start.second < page.characters.size() ? page.characters[start.second].top() * scale : 0);
}
QVariantMap DocumentPreview::linkAt(qreal x, qreal y, qreal contentWidth, qreal scale) const {
    const int n = pageAt(y, scale); if (n < 0 || scale <= 0) return {};
    const auto &page = m_pages[n];
    const QPointF point((x - (contentWidth - page.size.width() * scale) / 2) / (page.size.width() * scale),
        (y - pageY(n, scale)) / (page.size.height() * scale));
    for (const auto &value : page.links) if (value.toMap().value("area").toRectF().contains(point)) return value.toMap();
    return {};
}
void DocumentPreview::find(const QString &text, int fromPage, bool backwards) {
    if (!text.isEmpty()) search(text, fromPage, backwards, false);
}
void DocumentPreview::search(const QString &text, int fromPage, bool backwards, bool all) {
    if (m_searching || m_pages.isEmpty()) return;
    m_searching = true; emit searchChanged();
    const auto generation = m_generation;
    const auto source = m_source.toLocalFile();
    const auto anchor = m_anchor, cursor = m_cursor;
    const bool continuation = !all && selectedText().compare(text, Qt::CaseInsensitive) == 0;
    m_searchCancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_searchCancel;
    m_searchWorker.setFuture(QtConcurrent::run([source, generation, cancel, text, fromPage, backwards, all, anchor, cursor, continuation] {
        Result result; result.generation = generation;
        const auto response = requestDocument({{"source", source}, {"command", "search"}, {"text", text}, {"from", fromPage},
            {"backwards", backwards}, {"all", all}, {"continuation", continuation && fromPage == anchor.first},
            {"anchor", anchor.second}, {"cursor", cursor.second}}, cancel.get());
        result.error = response.value("error").toString();
        result.page = response.value("page", -1).toInt(); result.offset = response.value("offset", -1).toInt();
        result.length = response.value("all").toBool() ? -1 : response.value("length").toInt();
        for (const auto &page : response.value("pages").toList()) result.pages.append(decodedPage(page.toMap()));
        return result;
    }));
}
