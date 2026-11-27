#include "noteimport.h"
#include "../workspace/pilonworkspace.h"
#ifdef Q_OS_ANDROID
#include "documentpreview.h"
#include "../../platform/android/androidoffice.h"
#endif
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>
#include <algorithm>

namespace {
const QStringList officeFormats{"doc", "docx", "odt", "rtf", "txt", "html", "htm", "ppt", "pptx", "odp", "xls", "xlsx", "ods", "csv", "tsv"};
QString run(const QString &program, const QStringList &arguments, const std::shared_ptr<std::atomic_bool> &cancel) {
    if (program.isEmpty()) return QObject::tr("The document converter is unavailable.");
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("SAL_USE_VCLPLUGIN", "svp");
    process.setProcessEnvironment(environment);
    process.start(program, arguments);
    if (!process.waitForStarted()) return process.errorString();
    QElapsedTimer timer; timer.start();
    while (!process.waitForFinished(100)) {
        if (cancel->load() || timer.elapsed() > 300000) {
            process.kill(); process.waitForFinished();
            return cancel->load() ? QObject::tr("Import cancelled.") : QObject::tr("The document conversion timed out.");
        }
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return QObject::tr("Unable to import this document: %1").arg(QString::fromUtf8(process.readAllStandardError()).trimmed());
    return {};
}
}
bool NoteImport::supportsFile(const QString &path) {
    const QFileInfo file(path);
    return file.isFile() && (QImageReader(path).canRead() || file.suffix().compare("pdf", Qt::CaseInsensitive) == 0
        || officeFormats.contains(file.suffix().toLower()));
}
QString NoteImport::officeExecutable() {
#ifdef Q_OS_ANDROID
    return "android-office";
#else
    const auto configured = qEnvironmentVariable("PILON_NOTES_OFFICE");
    if (!configured.isEmpty() && QFileInfo(configured).isExecutable()) return configured;
    const auto bundled = QDir(PilonWorkspace::runtimeDirectory()).filePath("libreoffice/program/soffice");
    if (QFileInfo(bundled).isExecutable()) return bundled;
    return QStandardPaths::findExecutable("libreoffice");
#endif
}
QVariantMap NoteImport::pdf(const QString &path, const QString &destination, const QString &office,
    const std::shared_ptr<std::atomic_bool> &cancel) {
    if (QFileInfo(path).suffix().compare("pdf", Qt::CaseInsensitive) == 0) return {{"url", QUrl::fromLocalFile(path)}};
    if (!QDir().mkpath(destination)) return {{"error", QObject::tr("Unable to create the document preview folder.")}};
#ifdef Q_OS_ANDROID
    const auto output = QDir(destination).filePath(QFileInfo(path).completeBaseName() + ".pdf");
    const auto error = AndroidOffice::convert(path, output, cancel.get());
#else
    QTemporaryDir profile;
    const auto error = run(office, {"-env:UserInstallation=" + QUrl::fromLocalFile(profile.path()).toString(),
        "--headless", "--nologo", "--nodefault", "--norestore", "--convert-to", "pdf", "--outdir", destination, path}, cancel);
    const auto output = QDir(destination).filePath(QFileInfo(path).completeBaseName() + ".pdf");
#endif
    if (!error.isEmpty()) return {{"error", error}};
    if (!QFileInfo::exists(output)) return {{"error", QObject::tr("The document could not be converted.")}};
    return {{"url", QUrl::fromLocalFile(output)}};
}
QVariantMap NoteImport::files(const QVariantList &urls, const QString &assetRoot, const QString &job,
    qreal x, qreal y, qreal width, const QString &office, const std::shared_ptr<std::atomic_bool> &cancel) {
    const QString relative = "assets/" + job;
    const QString destination = QDir(assetRoot).filePath(relative);
    if (!QDir().mkpath(destination)) return {{"error", QObject::tr("Unable to create the note assets folder.")}};
    const auto failure = [&](const QString &message) {
        QDir(destination).removeRecursively(); return QVariantMap{{"error", message}};
    };
    QVariantList elements;
    int count = 0;
    const auto append = [&](const QString &source, const QString &name, bool page) -> QString {
        QImageReader reader(source); reader.setAutoTransform(true);
        const QString filename = QString("%1.png").arg(++count, 6, 10, QChar('0'));
        const auto path = QDir(destination).filePath(filename);
        QSize size;
        if (page) {
            size = reader.size();
            if (size.isEmpty()) return reader.errorString();
            if (!QFile::rename(source, path)) return QObject::tr("Unable to move the rendered page into the note.");
        } else {
            const auto image = reader.read();
            if (image.isNull()) return reader.errorString();
            size = image.size();
            QSaveFile output(path);
            if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") || !output.commit())
                return QObject::tr("Unable to save the imported image.");
        }
        const qreal displayWidth = page ? width : std::min(width, qreal(size.width()));
        const qreal height = displayWidth * size.height() / size.width();
        elements.append(QVariantMap{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"type", page ? "page" : "image"},
            {"asset", relative + "/" + filename}, {"name", name}, {"x", x}, {"y", y}, {"width", displayWidth}, {"height", height}});
        y += height + 24;
        return {};
    };
    for (const auto &value : urls) {
        if (cancel->load()) return failure(QObject::tr("Import cancelled."));
        const QUrl url = value.toUrl();
        if (!url.isLocalFile()) return failure(QObject::tr("Choose a local file to import."));
        const QString source = url.toLocalFile(), name = QFileInfo(source).fileName();
        if (!QFileInfo(source).isFile()) return failure(QObject::tr("The file is unavailable: %1").arg(name));
        QImageReader reader(source);
        if (reader.canRead()) {
            const auto error = append(source, name, false); if (!error.isEmpty()) return failure(error);
            continue;
        }
        QTemporaryDir stage(destination + "/.pages-XXXXXX");
        if (!stage.isValid()) return failure(QObject::tr("Unable to stage the imported pages."));
        const QString suffix = QFileInfo(source).suffix().toLower();
        if (suffix != "pdf" && !officeFormats.contains(suffix)) return failure(QObject::tr("Unsupported import format: %1").arg(name));
        const auto converted = NoteImport::pdf(source, stage.path(), office, cancel);
        if (converted.contains("error")) return failure(converted.value("error").toString());
        const auto pdf = converted.value("url").toUrl().toLocalFile();
#ifdef Q_OS_ANDROID
        const auto document = DocumentPreview::decodeDocument({{"source", pdf}, {"command", "open"}}, cancel.get());
        if (document.contains("error")) return failure(document.value("error").toString());
        const auto decoded = document.value("pages").toList();
        for (int i = 0; i < decoded.size(); ++i) {
            const auto output = stage.filePath(QString("page-%1.png").arg(i + 1));
            const auto page = DocumentPreview::decodeDocument({{"source", pdf}, {"command", "render"}, {"page", i},
                {"resolution", 1800}, {"output", output}, {"imageOnly", true}}, cancel.get());
            if (page.contains("error")) return failure(page.value("error").toString());
            const auto error = append(output, name, true);
            if (!error.isEmpty()) return failure(error);
        }
#else
        const QString error = run(QStandardPaths::findExecutable("pdftoppm"),
            {"-scale-to", "1800", "-png", pdf, stage.filePath("page")}, cancel);
        if (!error.isEmpty()) return failure(error);
        auto pages = QDir(stage.path()).entryList({"page-*.png"}, QDir::Files);
        std::sort(pages.begin(), pages.end(), [](const QString &a, const QString &b) {
            return a.mid(5).section('.', 0, 0).toInt() < b.mid(5).section('.', 0, 0).toInt();
        });
        if (pages.isEmpty()) return failure(QObject::tr("No pages were found in %1.").arg(name));
        for (const auto &page : pages) {
            if (cancel->load()) return failure(QObject::tr("Import cancelled."));
            const auto error = append(stage.filePath(page), name, true); if (!error.isEmpty()) return failure(error);
        }
#endif
    }
    if (cancel->load()) return failure(QObject::tr("Import cancelled."));
    return {{"elements", elements}};
}
