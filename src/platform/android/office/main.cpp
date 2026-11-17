#include <LibreOfficeKit.h>
#include <jni.h>
#include <dlfcn.h>
#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

namespace {
std::string string(JNIEnv *env, jstring value) {
    const char *bytes = env->GetStringUTFChars(value, nullptr);
    if (!bytes) return {};
    std::string result(bytes); env->ReleaseStringUTFChars(value, bytes); return result;
}
jbyteArray result(JNIEnv *env, const std::string &message) {
    const auto bytes = env->NewByteArray(message.size());
    if (bytes) env->SetByteArrayRegion(bytes, 0, message.size(), reinterpret_cast<const jbyte *>(message.data()));
    return bytes;
}
std::string error(LibreOfficeKit *office) {
    char *message = office->pClass->getError(office);
    const std::string value = message ? message : "Unable to convert this document.";
    if (message) office->pClass->freeError(message);
    return value.empty() ? "Unable to convert this document." : value;
}
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_org_pilon_PilonOffice_convertNative(JNIEnv *env, jclass,
    jstring data, jstring cache, jstring apk, jobject assets, jstring source, jstring destination) {
    try {
        static LibreOfficeKit *office = nullptr;
        if (!office) {
            auto library = dlopen("liblo-native-code.so", RTLD_NOW);
            if (!library) return result(env, "The document converter could not be loaded.");
            using Initialize = jboolean (*)(JNIEnv *, jstring, jstring, jstring, jobject);
            using Hook = LibreOfficeKit *(*)(const char *);
            const auto initialize = reinterpret_cast<Initialize>(dlsym(library, "libreofficekit_initialize"));
            const auto hook = reinterpret_cast<Hook>(dlsym(library, "libreofficekit_hook"));
            if (!initialize || !hook || !initialize(env, data, cache, apk, assets))
                return result(env, "Unable to initialize the document converter.");
            office = hook((string(env, data) + "/program").c_str());
            if (!office) return result(env, "Unable to initialize the document converter.");
        }
        const auto input = string(env, source), output = string(env, destination);
        const auto dot = input.find_last_of('.');
        std::string extension = dot == std::string::npos ? std::string{} : input.substr(dot);
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) { return char(std::tolower(value)); });
        // Batch mode avoids import dialogs; CSV keeps automatic delimiter/encoding detection.
        const char *options = extension == ".csv" ? "Batch=true,DETECT,34,DETECT,1,"
            : extension == ".tsv" ? "Batch=true,9,34,DETECT,1," : "Batch=true";
        auto destroy = [](LibreOfficeKitDocument *document) { document->pClass->destroy(document); };
        std::unique_ptr<LibreOfficeKitDocument, decltype(destroy)> document(office->pClass->documentLoadWithOptions(office, input.c_str(), options), destroy);
        if (!document) return result(env, error(office));
        if (!document->pClass->saveAs(document.get(), output.c_str(), "pdf", "")) return result(env, error(office));
        return result(env, {});
    } catch (...) {
        return result(env, "Unable to convert this document.");
    }
}
