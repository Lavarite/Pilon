// A shell-only harness for the production Android decoder; it never opens app data.
#include "documentpreview.h"
#include "noteimport.h"
#include <QDir>
#include <QImage>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QtMath>
#include <cstdio>
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    qputenv("TMPDIR", argv[1]);
    for (std::string line; std::getline(std::cin, line);) {
        const auto request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).toVariant().toMap();
        auto cancel = std::make_shared<std::atomic_bool>(request.value("cancelled").toBool());
        std::atomic_bool completed = false, saveObserved = false;
        std::thread canceller;
        if (request.value("cancelOnSave").toBool()) canceller = std::thread([&] {
            const auto output = request.value("output").toString();
            const auto directory = QFileInfo(output).absolutePath() + "/";
            while (!completed.load()) {
                for (const auto &fd : QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
                    const auto target = QFileInfo("/proc/self/fd/" + fd).symLinkTarget();
                    if (target.startsWith(directory) && (target.startsWith(output + ".") || target.endsWith(" (deleted)"))) {
                        saveObserved = true; *cancel = true; return;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        else if (request.value("cancelAfterMs").toInt() > 0) canceller = std::thread([cancel, request] {
            std::this_thread::sleep_for(std::chrono::milliseconds(request.value("cancelAfterMs").toInt()));
            *cancel = true;
        });
        QVariantMap result;
        if (request.value("command") == "concurrent") {
            auto rendering = request;
            rendering["command"] = "render"; rendering["page"] = 0; rendering["resolution"] = 3600;
            std::atomic_bool finished = false;
            QVariantMap rendered;
            std::thread renderer([&] { rendered = DocumentPreview::decodeDocument(rendering, cancel.get()); finished = true; });
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            QElapsedTimer timer; timer.start();
            result = DocumentPreview::decodeDocument({{"source", request.value("searchSource")}, {"command", "search"},
                {"text", "selectable"}, {"from", 0}, {"all", false}}, cancel.get());
            result["searchMilliseconds"] = timer.elapsed(); result["renderStillRunning"] = !finished.load();
            *cancel = true; renderer.join(); result["renderError"] = rendered.value("error");
        } else if (request.value("command") == "import") {
            QVariantList urls;
            for (const auto &source : request.value("sources").toList()) urls.append(QUrl::fromLocalFile(source.toString()));
            result = NoteImport::files(urls, request.value("destination").toString(), request.value("job").toString(), 32, 120, 600, {}, cancel);
        } else result = DocumentPreview::decodeDocument(request, cancel.get());
        completed = true;
        if (canceller.joinable()) canceller.join();
        if (request.value("cancelOnSave").toBool()) result["saveObserved"] = saveObserved.load();
        if (request.value("command") == "render" && !result.contains("error")) {
            const QImage image(request.value("output").toString());
            bool ink = false;
            const auto boxes = result.value("characters").toList();
            if (!boxes.isEmpty()) {
                const auto box = boxes.first().toList();
                const auto width = request.value("pageWidth").toDouble(), height = request.value("pageHeight").toDouble();
                if (width > 0 && height > 0 && box.size() == 4) {
                    const QRect rectangle(qFloor(box[0].toDouble() * image.width() / width) - 1,
                        qFloor(box[1].toDouble() * image.height() / height) - 1,
                        qCeil(box[2].toDouble() * image.width() / width) + 2,
                        qCeil(box[3].toDouble() * image.height() / height) + 2);
                    for (int y = std::max(0, rectangle.top()); y <= std::min(image.height() - 1, rectangle.bottom()); ++y)
                        for (int x = std::max(0, rectangle.left()); x <= std::min(image.width() - 1, rectangle.right()); ++x)
                            if (qGray(image.pixel(x, y)) < 100) ink = true;
                }
            }
            result["testImage"] = QVariantMap{{"width", image.width()}, {"height", image.height()}, {"firstCharacterHasInk", ink}};
        }
        const auto json = QJsonDocument::fromVariant(result).toJson(QJsonDocument::Compact);
        fwrite(json.data(), 1, json.size(), stdout); puts(""); fflush(stdout);
    }
}
