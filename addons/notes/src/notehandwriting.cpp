#include "notehandwriting.h"
#include "notedocument.h"
#include "../../../src/core/workspace/pilonworkspace.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPainter>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>

QString NoteHandwriting::fingerprint(const QVariantList &strokes) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument::fromVariant(strokes).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
namespace {
QString lineKey(const QStringList &members) {
    auto sorted = members; std::sort(sorted.begin(), sorted.end());
    return QString::fromLatin1(QCryptographicHash::hash(sorted.join(':').toLatin1(), QCryptographicHash::Sha256).toHex());
}
}
QVariantList NoteHandwriting::renderLines(const QVariantList &strokes, const QString &directory) {
    Cache cache; return prepareLines(strokes, directory, cache);
}
QVariantList NoteHandwriting::prepareLines(const QVariantList &strokes, const QString &directory, Cache &cache, const QVariantList &previousLines, const QVariantList &elements, const QString &assetRoot) {
    for (const auto &value : previousLines) {
        const auto line = value.toMap();
        if (line.contains("key") && line.contains("detections"))
            cache.tiles.insert(line.value("key").toString(), line.value("detections").toList());
    }
    // Spatial tiles keep drawings from merging unrelated rows into a giant line.
    // The overlap keeps words at tile boundaries intact. Coordinates stay in note units.
    constexpr int side = 640, margin = 128;
    QMap<QPair<int, int>, QVariantList> tiles;
    QMap<QPair<int, int>, QStringList> members;
    for (const auto &value : strokes) {
        const auto box = NoteDocument::strokeBounds(value.toMap());
        // Keep page-spanning drawing strokes out of the text raster, as before.
        if (box.height() > 400 || box.width() > 2000) continue;
        const auto key = fingerprint({value});
        for (int y = int(std::floor((box.top()-margin)/side)); y <= int(std::floor((box.bottom()+margin)/side)); ++y)
            for (int x = int(std::floor((box.left()-margin)/side)); x <= int(std::floor((box.right()+margin)/side)); ++x) {
                const QPair<int, int> cell{x, y};
                tiles[cell].append(value); members[cell].append(key);
            }
    }
    QMap<QPair<int, int>, QVariantList> imageTiles;
    QHash<QString, QImage> images;
    for (const auto &value : elements) {
        const auto element = value.toMap(); if (element.value("type") != "image" && element.value("type") != "page") continue;
        const auto asset = element.value("asset").toString();
        if (!asset.startsWith("assets/") || QDir::cleanPath(asset) != asset || asset.contains("../")) continue;
        const QImage image(QDir(assetRoot).filePath(asset)); if (image.isNull()) continue;
        images.insert(asset, image);
        const auto box = NoteDocument::elementTransform(element).mapRect(QRectF(0, 0, element.value("width").toDouble(), element.value("height").toDouble()));
        // Hash source pixels, not QImage's allocation key, so saved tile results remain reusable.
        const auto imageKey = fingerprint({value}) + QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromRawData(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes()), QCryptographicHash::Sha256).toHex());
        for (int y = int(std::floor(box.top()/side)); y <= int(std::floor(box.bottom()/side)); ++y)
            for (int x = int(std::floor(box.left()/side)); x <= int(std::floor(box.right()/side)); ++x) {
                const QPair<int, int> cell{x, y}; tiles[cell]; imageTiles[cell].append(value); members[cell].append(imageKey);
            }
    }
    QVariantList result;
    QHash<QString, QVariantList> retained;
    for (auto it = tiles.cbegin(); it != tiles.cend(); ++it) {
        const auto cell = it.key();
        const qreal x = cell.first*side, y = cell.second*side;
        const QRectF core(x, y, side, side), box = core.adjusted(-margin, -margin, margin, margin);
        // Do not schedule padding-only cells.
        if (imageTiles.value(cell).isEmpty() && std::none_of(it.value().cbegin(), it.value().cend(), [&](const QVariant &v) {
            return core.intersects(NoteDocument::strokeBounds(v.toMap()));
        })) continue;
        auto keys = members.value(cell); keys.append(QString::number(x) + "," + QString::number(y));
        const auto key = lineKey(keys);
        constexpr qreal scale = 1.5;
        QVariantMap entry{{"key", key}, {"x", box.x()}, {"y", box.y()},
            {"width", box.width()}, {"height", box.height()}, {"coreX", x}, {"coreY", y},
            {"coreSize", side}, {"scale", scale}};
        if (cache.tiles.contains(key)) {
            entry["detections"] = cache.tiles.value(key); retained.insert(key, cache.tiles.value(key));
        } else {
            QImage image(QSize(qCeil(box.width()*scale), qCeil(box.height()*scale)), QImage::Format_RGB32); image.fill(Qt::white);
            QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing); painter.scale(scale, scale); painter.translate(-box.topLeft());
            for (const auto &value : imageTiles.value(cell)) {
                const auto element = value.toMap(); painter.save();
                painter.setTransform(NoteDocument::elementTransform(element), true);
                painter.drawImage(QRectF(0, 0, element.value("width").toDouble(), element.value("height").toDouble()), images.value(element.value("asset").toString()));
                painter.restore();
            }
            for (const auto &value : it.value()) { auto stroke = value.toMap(); stroke["colour"] = "#000000"; NoteDocument::paintStroke(painter, stroke); }
            painter.end();
            const auto path = QDir(directory).filePath(key + ".png");
            if (!image.save(path)) continue;
            entry["path"] = path;
        }
        result.append(entry);
    }
    cache.tiles = retained;
    return result;
}

NoteHandwriting::NoteHandwriting(QObject *parent) : QObject(parent) {
    m_root = qEnvironmentVariable("PILON_NOTES_OCR_ROOT", QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(".."));
    m_delay.setSingleShot(true); m_delay.setInterval(600);
    m_timeout.setSingleShot(true); m_timeout.setInterval(120000);
    connect(&m_delay, &QTimer::timeout, this, &NoteHandwriting::next);
    connect(&m_timeout, &QTimer::timeout, this, [this] { emit failed(tr("Handwriting search timed out.")); m_process.kill(); });
    connect(&m_render, &QFutureWatcher<QVariantList>::finished, this, [this] {
        if (!m_active || m_id.isEmpty()) return;
        m_lines = m_render.result();
        QVariantList pending;
        for (const auto &value : m_lines) if (value.toMap().contains("path")) pending.append(value);
        if (pending.isEmpty()) { publish(); finish(); return; }
        m_request = QJsonDocument::fromVariant(QVariantMap{{"lines", pending}}).toJson(QJsonDocument::Compact) + '\n';
        if (m_process.state() == QProcess::Running) send();
        else {
#ifdef Q_OS_ANDROID
            m_process.setProgram(PilonWorkspace::packagedExecutable("ocr"));
            m_process.setArguments({PilonWorkspace::modelsDirectory() + "/ppocrv5"});
#else
            m_process.setProgram(m_root + "/runtime/handwriting/bin/python");
            m_process.setArguments({"-u", m_directory.filePath("handwriting_worker.py"), m_root + "/models/ppocrv5"});
#endif
            auto env = QProcessEnvironment::systemEnvironment(); env.insert("HF_HUB_OFFLINE", "1"); env.insert("TOKENIZERS_PARALLELISM", "false");
#ifdef Q_OS_ANDROID
            env.insert("LD_LIBRARY_PATH", QFileInfo(m_process.program()).absolutePath());
#endif
            m_process.setProcessEnvironment(env); m_process.start();
        }
    });
    connect(&m_process, &QProcess::started, this, &NoteHandwriting::send);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        if (!m_active) { m_process.readAllStandardOutput(); return; }
        m_output += m_process.readAllStandardOutput();
        while (m_output.contains('\n')) {
            const int end = m_output.indexOf('\n'); const auto data = QJsonDocument::fromJson(m_output.left(end)).toVariant().toMap(); m_output.remove(0, end + 1);
            if (data.contains("progress")) { m_timeout.start(); continue; }
            if (data.contains("error")) emit failed(tr("Handwriting search: %1").arg(data.value("error").toString()));
            else if (data.contains("tiles")) {
                const auto tiles = data.value("tiles").toList();
                if (tiles.size() != std::count_if(m_lines.cbegin(), m_lines.cend(), [](const QVariant &v) { return v.toMap().contains("path"); })) {
                    emit failed(tr("Invalid handwriting result.")); finish(); continue;
                }
                int i = 0;
                auto cache = m_caches.value(m_id);
                for (auto &value : m_lines) {
                    auto line = value.toMap(); if (!line.contains("path")) continue;
                    line.remove("path"); line["detections"] = tiles.value(i++).toList(); value = line;
                    if (cache) cache->tiles.insert(line.value("key").toString(), line.value("detections").toList());
                }
                publish();
            } else continue;
            finish();
        }
    });
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        const auto diagnostics = m_process.readAllStandardError();
        if (qEnvironmentVariableIsSet("PILON_INPUT_DEBUG")) qInfo().noquote() << QString::fromUtf8(diagnostics);
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) { if (error == QProcess::FailedToStart) { emit failed(tr("Unable to start handwriting search.")); finish(); } });
    connect(&m_process, &QProcess::finished, this, [this] { if (!m_id.isEmpty()) { emit failed(tr("The handwriting recognizer stopped.")); finish(); } });
}
NoteHandwriting::~NoteHandwriting() { disconnect(&m_process, nullptr, this, nullptr); m_process.kill(); m_process.waitForFinished(1000); m_render.waitForFinished(); }
void NoteHandwriting::setActive(bool active) {
    if (m_active == active) return;
    m_active = active;
    if (active) return;
    m_delay.stop(); m_timeout.stop(); m_pending.clear(); m_id.clear(); m_output.clear(); m_request.clear();
    m_process.kill(); m_process.waitForFinished(1000);
    m_render.waitForFinished(); m_lines.clear();
}
void NoteHandwriting::setWriting(bool writing) {
    m_writing = writing;
    if (!m_active) return;
    if (writing) m_delay.stop(); else if (!m_writing && !m_pending.isEmpty()) m_delay.start();
}
void NoteHandwriting::enqueue(const QString &id, const QVariantList &strokes, qint64 revision, const QVariantList &previousLines, const QVariantList &elements, const QString &assetRoot) {
    if (!m_active) return;
    if (strokes.isEmpty() && std::none_of(elements.cbegin(), elements.cend(), [](const QVariant &v) { return (v.toMap().value("type") == "image" || v.toMap().value("type") == "page"); })) { m_pending.remove(id); m_caches.remove(id); emit indexed(id, revision, {}); return; }
    if (qEnvironmentVariable("PILON_NOTES_HANDWRITING") == "0") return;
    m_pending[id] = {strokes, previousLines, revision, elements, assetRoot}; if (!m_writing) m_delay.start();
}
void NoteHandwriting::next() {
    if (!m_active || m_writing || !m_id.isEmpty() || m_pending.isEmpty()) return;
#ifdef Q_OS_ANDROID
    if (PilonWorkspace::packagedExecutable("ocr").isEmpty()) {
        m_pending.clear(); emit failed(tr("Handwriting recognition is missing from this installation.")); return;
    }
    const auto models = PilonWorkspace::modelsDirectory() + "/ppocrv5/";
    if (!QFile::exists(models + "en_PP-OCRv5_rec_mobile.onnx") || !QFile::exists(models + "ch_PP-OCRv5_det_server.onnx")) {
        m_pending.clear(); emit failed(tr("Download Handwriting recognition in Settings → Models.")); return;
    }
#else
    if (!QFile::exists(m_root + "/runtime/handwriting/bin/python") || !QFile::exists(m_root + "/models/ppocrv5/en_PP-OCRv5_rec_mobile.onnx")) {
        m_pending.clear(); emit failed(tr("Handwriting search needs its runtime and models. Run ./src/scripts/build.sh for the runtime, then download Handwriting recognition in Settings → Models.")); return;
    }
    if (!QFile::exists(m_directory.filePath("handwriting_worker.py")) && !QFile::copy(":/qt/qml/Pilon/handwriting_worker.py", m_directory.filePath("handwriting_worker.py"))) {
        m_pending.clear(); emit failed(tr("Unable to prepare handwriting search.")); return;
    }
#endif
    const auto it = m_pending.begin(); m_id = it.key(); const auto job = it.value(); m_pending.erase(it); m_revision = job.revision;
    auto cache = m_caches.value(m_id);
    if (!cache) { cache = std::make_shared<Cache>(); m_caches.insert(m_id, cache); }
    // Bound retained documents; line recognition results also persist in the note.
    while (m_caches.size() > 3) { auto old = m_caches.begin(); if (old.key() == m_id) ++old; m_caches.erase(old); }
    const auto directory = m_directory.path();
    m_render.setFuture(QtConcurrent::run([job, directory, cache] { return prepareLines(job.strokes, directory, *cache, job.previousLines, job.elements, job.assetRoot); }));
}
void NoteHandwriting::publish() {
    if (!m_active) return;
    // Retain per-tile detections for incremental reuse, but keep text lines flat
    // for library search and older readers.
    QVariantList result;
    for (const auto &value : m_lines) {
        const auto tile = value.toMap();
        result.append(QVariantMap{{"key", tile.value("key")}, {"detections", tile.value("detections")}});
        result += tile.value("detections").toList();
    }
    emit indexed(m_id, m_revision, result);
}
void NoteHandwriting::send() { if (!m_active) return; m_timeout.start(); m_process.write(m_request); m_request.clear(); }
void NoteHandwriting::finish() {
    m_timeout.stop(); m_id.clear(); m_lines.clear();
    for (const auto &path : QDir(m_directory.path()).entryList({"*.png"}, QDir::Files)) QFile::remove(m_directory.filePath(path));
    if (m_active && !m_writing && !m_pending.isEmpty()) m_delay.start();
}
