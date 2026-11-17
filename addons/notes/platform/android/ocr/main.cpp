// Adapted from RapidAI's Apache-2.0 Android inference and RapidOCR CTC/box code; see NOTICE.txt.
#include "OcrUtils.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numeric>
#include <stdexcept>

std::vector<TextBox> findRsBoxes(const cv::Mat &, const cv::Mat &, ScaleParam &, float, float);
namespace {
struct Glyph { QString text; int column; int start; float score; };
struct Recognition { QString text; std::vector<Glyph> glyphs; float score = 0; int columns = 0; };

Recognition decode(const float *data, int steps, int classes, const QStringList &keys) {
    if (steps <= 0 || classes != keys.size()) throw std::runtime_error("Invalid recognition tensor dimensions.");
    Recognition result; result.columns = steps;
    int previous = -1;
    for (int step = 0; step < steps; ++step) {
        const auto row = data + step * classes;
        const int index = int(std::max_element(row, row + classes) - row);
        if (index && index != previous) {
            const float score = std::round(row[index] * 100000.f) / 100000.f;
            result.glyphs.push_back({keys[index], step, int(result.text.size()), score});
            result.text += keys[index]; result.score += score;
        }
        previous = index;
    }
    if (!result.glyphs.empty()) result.score /= result.glyphs.size();
    return result;
}

std::vector<std::array<double, 2>> characterCells(const Recognition &text, int width, int height) {
    const double ratio = double(width) / height;
    const double lineLength = text.columns * ratio / std::max(320. / 48, ratio);
    if (lineLength <= 0 || text.text.isEmpty()) return {};
    const double columnWidth = width / lineLength;
    std::vector<double> widths;
    std::vector<int> word;
    const auto finishWord = [&] {
        if (word.size() > 1) widths.push_back((word.back() - word.front()) * columnWidth / (word.size() - 1));
        word.clear();
    };
    for (const auto &glyph : text.glyphs) {
        if (glyph.text.trimmed().isEmpty()) { finishWord(); continue; }
        if (!word.empty() && glyph.column - word.back() > 5) finishWord();
        word.push_back(glyph.column);
    }
    finishWord();
    const double characterWidth = widths.empty() ? double(width) / text.text.toUcs4().size()
        : std::accumulate(widths.begin(), widths.end(), 0.) / widths.size();
    std::vector<std::array<double, 2>> cells;
    for (const auto &glyph : text.glyphs) {
        if (glyph.text.trimmed().isEmpty()) continue;
        const double center = (glyph.column + .5) * columnWidth;
        cells.push_back({double(std::max(int(center - characterWidth / 2), 0)),
                         double(std::min(int(center + characterWidth / 2), width))});
    }
    for (size_t i = 1; i < cells.size(); ++i) {
        if (cells[i - 1][1] <= cells[i][0]) continue;
        const double middle = (cells[i - 1][1] + cells[i][0]) / 2;
        cells[i - 1][1] = cells[i][0] = middle;
    }
    return cells;
}

QJsonObject bounds(const std::vector<cv::Point2f> &points, const QJsonObject &tile,
                   double ratioX, double ratioY, int padding = 0) {
    double left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
    for (const auto &point : points) {
        const double x = int(point.x) * ratioX, y = (int(point.y) - padding) * ratioY;
        left = std::min(left, x); right = std::max(right, x);
        top = std::min(top, y); bottom = std::max(bottom, y);
    }
    const double scale = tile["scale"].toDouble();
    return {{"x", left / scale + tile["x"].toDouble()}, {"y", top / scale + tile["y"].toDouble()},
            {"width", (right - left) / scale}, {"height", (bottom - top) / scale}};
}

class Model {
    Ort::Session session;
    std::vector<Ort::AllocatedStringPtr> inputs, outputs;
public:
    Model(Ort::Env &environment, const std::string &path, const Ort::SessionOptions &options)
        : session(environment, path.c_str(), options), inputs(getInputNames(&session)), outputs(getOutputNames(&session)) {}
    Ort::Value run(std::vector<float> &pixels, const std::array<int64_t, 4> &shape) {
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(memory, pixels.data(), pixels.size(), shape.data(), shape.size());
        const char *input = inputs.at(0).get(), *output = outputs.at(0).get();
        auto result = session.Run(Ort::RunOptions{nullptr}, &input, &tensor, 1, &output, 1);
        if (result.size() != 1 || result[0].GetTensorTypeAndShapeInfo().GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            throw std::runtime_error("Invalid OCR model output.");
        return std::move(result[0]);
    }
    QStringList characters() const {
        Ort::AllocatorWithDefaultOptions allocator;
        auto value = session.GetModelMetadata().LookupCustomMetadataMapAllocated("character", allocator);
        if (!value || !value.get()[0]) throw std::runtime_error("The recognition model has no character dictionary.");
        auto keys = QString::fromUtf8(value.get()).split('\n');
        if (keys.last().isEmpty()) keys.removeLast();
        for (auto &key : keys) if (key.endsWith('\r')) key.chop(1);
        keys.prepend(QString{}); keys.append(QStringLiteral(" ")); return keys;
    }
};

class Engine {
    Ort::Env environment{ORT_LOGGING_LEVEL_ERROR, "Pilon OCR"};
    Ort::SessionOptions options;
    std::unique_ptr<Model> detector, recognizer;
    QStringList keys;
public:
    explicit Engine(const std::string &root) {
        options.SetIntraOpNumThreads(2); options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        detector = std::make_unique<Model>(environment, root + "/ch_PP-OCRv5_det_server.onnx", options);
        recognizer = std::make_unique<Model>(environment, root + "/en_PP-OCRv5_rec_mobile.onnx", options);
        keys = recognizer->characters();
    }
    Recognition recognize(const cv::Mat &crop) {
        constexpr int height = 48;
        const double ratio = double(crop.cols) / crop.rows;
        const int width = int(height * std::max(320. / height, ratio));
        if (width <= 0 || width > 16384) throw std::runtime_error("The text region is too wide.");
        const int resizedWidth = std::min(width, int(std::ceil(height * ratio)));
        cv::Mat resized; cv::resize(crop, resized, cv::Size(resizedWidth, height));
        std::vector<float> pixels(size_t(3) * height * width, 0);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < resizedWidth; ++x)
                for (int channel = 0; channel < 3; ++channel)
                    pixels[(channel * height + y) * width + x] = resized.at<cv::Vec3b>(y, x)[channel] / 127.5f - 1.f;
        auto output = recognizer->run(pixels, {1, 3, height, width});
        const auto shape = output.GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 3 || shape[0] != 1 || shape[1] > 16384 || shape[2] != keys.size())
            throw std::runtime_error("The recognition model has an invalid output shape.");
        return decode(output.GetTensorData<float>(), int(shape[1]), int(shape[2]), keys);
    }
    QJsonArray recognizeTile(const QJsonObject &tile) {
        const double scale = tile["scale"].toDouble(), coreSize = tile["coreSize"].toDouble();
        for (const char *key : {"scale", "x", "y", "coreX", "coreY", "coreSize"})
            if (!tile[key].isDouble() || !std::isfinite(tile[key].toDouble())) throw std::runtime_error("Invalid OCR tile coordinates.");
        if (scale <= 0 || coreSize <= 0 || !tile["path"].isString()) throw std::runtime_error("Invalid OCR tile.");
        cv::Mat image = cv::imread(tile["path"].toString().toStdString(), cv::IMREAD_COLOR);
        if (image.empty() || image.total() > 16000000) throw std::runtime_error("Unable to read the OCR tile image.");
        const cv::Size original = image.size();
        const auto multiple32 = [](double value) { return std::max(32, int(std::nearbyint(int(value) / 32.)) * 32); };
        if (std::max(image.cols, image.rows) > 768) {
            const double ratio = 768. / std::max(image.cols, image.rows);
            cv::resize(image, image, cv::Size(multiple32(image.cols * ratio), multiple32(image.rows * ratio)));
        }
        const double ratioX = double(original.width) / image.cols, ratioY = double(original.height) / image.rows;
        int padding = 0;
        if (image.rows <= 30 || double(image.cols) / image.rows > 8) {
            padding = std::abs(std::max(int(image.cols / 8.), 30) * 2 - image.rows) / 2;
            cv::copyMakeBorder(image, image, padding, padding, 0, 0, cv::BORDER_CONSTANT, cv::Scalar(255, 255, 255));
        }
        const double resizeRatio = std::max(1., 736. / std::min(image.cols, image.rows));
        ScaleParam geometry{image.cols, image.rows, multiple32(image.cols * resizeRatio), multiple32(image.rows * resizeRatio), 0, 0};
        geometry.ratioWidth = float(geometry.dstWidth) / image.cols; geometry.ratioHeight = float(geometry.dstHeight) / image.rows;
        cv::Mat resized; cv::resize(image, resized, cv::Size(geometry.dstWidth, geometry.dstHeight));
        const float mean[3]{127.5, 127.5, 127.5}, norm[3]{1.f / 127.5f, 1.f / 127.5f, 1.f / 127.5f};
        auto pixels = substractMeanNormalize(resized, mean, norm);
        auto output = detector->run(pixels, {1, 3, resized.rows, resized.cols});
        const auto shape = output.GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 4 || shape[0] != 1 || shape[1] != 1 || shape[2] != resized.rows || shape[3] != resized.cols)
            throw std::runtime_error("The detection model has an invalid output shape.");
        cv::Mat prediction(resized.rows, resized.cols, CV_32F, output.GetTensorMutableData<float>()), mask;
        cv::compare(prediction, .3, mask, cv::CMP_GT);
        cv::dilate(mask, mask, cv::getStructuringElement(cv::MORPH_RECT, {2, 2}));
        auto boxes = findRsBoxes(prediction, mask, geometry, .5, 1.6);
        std::stable_sort(boxes.begin(), boxes.end(), [](const TextBox &a, const TextBox &b) { return a.boxPoint[0].y < b.boxPoint[0].y; });
        for (size_t first = 0; first < boxes.size();) {
            size_t last = first + 1;
            while (last < boxes.size() && boxes[last].boxPoint[0].y - boxes[last - 1].boxPoint[0].y < 10) ++last;
            std::stable_sort(boxes.begin() + first, boxes.begin() + last, [](const TextBox &a, const TextBox &b) { return a.boxPoint[0].x < b.boxPoint[0].x; });
            first = last;
        }
        QJsonArray lines;
        for (const auto &box : boxes) {
            std::vector<cv::Point2f> points; for (const auto &p : box.boxPoint) points.emplace_back(p.x, p.y);
            const auto region = bounds(points, tile, ratioX, ratioY, padding);
            const double cx = region["x"].toDouble() + region["width"].toDouble() / 2;
            const double cy = region["y"].toDouble() + region["height"].toDouble() / 2;
            if (cx < tile["coreX"].toDouble() || cx >= tile["coreX"].toDouble() + coreSize
                || cy < tile["coreY"].toDouble() || cy >= tile["coreY"].toDouble() + coreSize) continue;
            const int width = int(std::max(cv::norm(points[0] - points[1]), cv::norm(points[2] - points[3])));
            const int height = int(std::max(cv::norm(points[0] - points[3]), cv::norm(points[1] - points[2])));
            if (width < 3 || height < 3) continue;
            const std::vector<cv::Point2f> rectangle{{0, 0}, {float(width), 0}, {float(width), float(height)}, {0, float(height)}};
            const auto transform = cv::getPerspectiveTransform(points, rectangle);
            cv::Mat crop; cv::warpPerspective(image, crop, transform, {width, height}, cv::INTER_CUBIC, cv::BORDER_REPLICATE);
            const bool vertical = double(height) / width >= 1.5;
            if (vertical) cv::rotate(crop, crop, cv::ROTATE_90_COUNTERCLOCKWISE);
            const auto text = recognize(crop);
            if (text.text.isEmpty() || text.score < .5) continue;
            const auto cells = characterCells(text, crop.cols, crop.rows);
            QJsonArray words; size_t cell = 0;
            for (const auto &glyph : text.glyphs) {
                if (glyph.text.trimmed().isEmpty()) continue;
                const auto [left, right] = cells.at(cell++);
                if (right <= left) continue;
                std::vector<cv::Point2f> character{{float(left), 0}, {float(right), 0}, {float(right), float(crop.rows)}, {float(left), float(crop.rows)}};
                if (vertical) for (auto &point : character) point = cv::Point2f(width - point.y, point.x);
                cv::perspectiveTransform(character, character, transform.inv());
                auto word = bounds(character, tile, ratioX, ratioY, padding);
                word.insert("start", glyph.start); word.insert("length", int(glyph.text.size())); words.append(word);
            }
            auto line = region; line.insert("text", text.text); line.insert("score", text.score); line.insert("words", words); lines.append(line);
        }
        return lines;
    }
};

void selfTest() {
    const QStringList keys{QString{}, QStringLiteral("A"), QString::fromUtf8("😀"), QString::fromUtf8("é"), QStringLiteral(" ")};
    const std::vector<int> tokens{1, 1, 0, 2, 0, 3, 4, 1, 0};
    std::vector<float> scores(tokens.size() * keys.size(), .001f);
    for (size_t i = 0; i < tokens.size(); ++i) scores[i * keys.size() + tokens[i]] = .99;
    const auto text = decode(scores.data(), int(tokens.size()), int(keys.size()), keys);
    if (text.text != QString::fromUtf8("A😀é A") || text.glyphs.size() != 5
        || text.glyphs[1].start != 1 || text.glyphs[2].start != 3 || text.glyphs[4].start != 5
        || text.glyphs[1].text.size() != 2) throw std::runtime_error("CTC UTF-16 self-check failed.");
    const auto cells = characterCells(text, 320, 48);
    if (cells.size() != 4 || cells[1][0] <= cells[0][1] || cells[3][1] > 320)
        throw std::runtime_error("CTC character bounds self-check failed.");
    std::cout << "CTC positions, duplicate/blank decoding and UTF-16 offsets passed.\n";
}
}

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--self-test") { selfTest(); return 0; }
        if (argc != 2) throw std::runtime_error("Usage: libpilon_ocr.so MODEL_DIRECTORY");
        Engine engine(argv[1]);
        std::string line;
        while (std::getline(std::cin, line)) {
            try {
                if (line.size() > 16 * 1024 * 1024) throw std::runtime_error("OCR request is too large.");
                QJsonParseError error;
                const auto request = QJsonDocument::fromJson(QByteArray::fromStdString(line), &error);
                if (error.error != QJsonParseError::NoError || !request.isObject() || !request.object()["lines"].isArray())
                    throw std::runtime_error("Invalid OCR request.");
                const auto tiles = request.object()["lines"].toArray();
                if (tiles.size() > 128) throw std::runtime_error("Too many OCR tiles.");
                QJsonArray results;
                for (const auto &tile : tiles) {
                    results.append(engine.recognizeTile(tile.toObject()));
                    std::cout << QJsonDocument(QJsonObject{{"progress", results.size()}}).toJson(QJsonDocument::Compact).constData() << std::endl;
                }
                std::cout << QJsonDocument(QJsonObject{{"tiles", results}}).toJson(QJsonDocument::Compact).constData() << std::endl;
            } catch (const std::exception &error) {
                std::cout << QJsonDocument(QJsonObject{{"error", QString::fromUtf8(error.what())}}).toJson(QJsonDocument::Compact).constData() << std::endl;
            }
        }
    } catch (const std::exception &error) {
        std::cout << QJsonDocument(QJsonObject{{"error", QString::fromUtf8(error.what())}}).toJson(QJsonDocument::Compact).constData() << std::endl;
        return 1;
    }
}
