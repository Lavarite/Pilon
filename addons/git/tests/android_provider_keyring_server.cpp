// Host fixture for the Android provider protocol; it never opens application credentials.
#include "gitcredentialserver.h"
#include <QCoreApplication>
#include <QHash>
#include <QTextStream>

int main(int argc, char **argv) {
    QCoreApplication::setApplicationName("pilon-provider-proof");
    QCoreApplication::setApplicationVersion("1");
    QCoreApplication application(argc, argv);
    if (argc != 4) return 2;
    QHash<QString, QByteArray> values;
    GitCredentialServer server(QString::fromLocal8Bit(argv[1]),
        [&](QString action, QByteArray request, GitCredentialServer::Completed completed) {
            const auto object = QJsonDocument::fromJson(request).object();
            const auto service = object.value("service").toString();
            const auto user = object.value("user").toString();
            const auto key = service + QChar::Null + user;
            const auto operation = object.value("action").toString();
            bool found = values.contains(key);
            QByteArray secret;
            if (operation == "store") {
                auto decoded = QByteArray::fromBase64Encoding(object.value("secret").toString().toLatin1(),
                    QByteArray::AbortOnBase64DecodingErrors);
                if (!decoded) { completed({}); return; }
                values.insert(key, std::move(decoded.decoded));
                found = true;
            } else if (operation == "lookup") {
                secret = values.value(key);
            } else if (operation == "clear") {
                if (object.value("all").toBool()) {
                    const auto prefix = service + QChar::Null;
                    for (auto entry = values.begin(); entry != values.end();) {
                        if (entry.key().startsWith(prefix)) entry = values.erase(entry);
                        else ++entry;
                    }
                } else values.remove(key);
            }
            const auto output = QJsonDocument(QJsonObject{{"found", found},
                {"secret", QString::fromLatin1(secret.toBase64())}}).toJson(QJsonDocument::Compact);
            completed({action == "keyring", output, {}});
        });
    if (server.address().isEmpty()
        || !server.setProviderScope(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]))) return 3;
    QTextStream(stdout) << "READY " << QCoreApplication::applicationPid() << '\n' << Qt::flush;
    QTimer::singleShot(60000, &application, &QCoreApplication::quit);
    return application.exec();
}
