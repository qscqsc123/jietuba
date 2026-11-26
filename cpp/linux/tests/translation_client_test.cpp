#include "translation_client.h"
#include <QHostAddress>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QtTest>

Q_DECLARE_METATYPE(LinuxTranslationResult)

class TranslationClientTest final : public QObject {
    Q_OBJECT
private slots:
    void validatesAndBuildsRequests() {
        const auto missing = LinuxTranslationClient::prepare(QStringLiteral("Hello"), {QStringLiteral("deepl_free"), {}, {}, QStringLiteral("zh")});
        QVERIFY(!missing.error.isEmpty());
        const auto deepl = LinuxTranslationClient::prepare(QStringLiteral("Hello"), {QStringLiteral("deepl_free"), QStringLiteral("test-key"), {}, QStringLiteral("zh")});
        QVERIFY(deepl.error.isEmpty());
        QCOMPARE(deepl.request.url().toString(), QStringLiteral("https://api-free.deepl.com/v2/translate"));
        QCOMPARE(deepl.request.rawHeader("Authorization"), QByteArray("DeepL-Auth-Key test-key"));
        QVERIFY(!deepl.request.url().toString().contains(QStringLiteral("test-key")));
        const auto deeplBody = QJsonDocument::fromJson(deepl.body).object();
        QCOMPARE(deeplBody.value(QStringLiteral("target_lang")).toString(), QStringLiteral("ZH"));
        QCOMPARE(deeplBody.value(QStringLiteral("text")).toArray().first().toString(), QStringLiteral("Hello"));
        const auto libre = LinuxTranslationClient::prepare(QStringLiteral("Hello"), {QStringLiteral("libretranslate"), {}, QStringLiteral("http://127.0.0.1:5000/translate"), QStringLiteral("en")});
        QVERIFY(libre.error.isEmpty());
        const auto libreBody = QJsonDocument::fromJson(libre.body).object();
        QCOMPARE(libreBody.value(QStringLiteral("source")).toString(), QStringLiteral("auto"));
        QCOMPARE(libreBody.value(QStringLiteral("target")).toString(), QStringLiteral("en"));
        QVERIFY(!libreBody.contains(QStringLiteral("api_key")));
        QVERIFY(!LinuxTranslationClient::prepare(QStringLiteral("Hello"), {QStringLiteral("libretranslate"), {}, QStringLiteral("file:///tmp/translate"), QStringLiteral("en")}).error.isEmpty());
        LinuxTranslationConfig googleConfig{QStringLiteral("google"), {}, {}, QStringLiteral("zh"), QStringLiteral("google-key")};
        const auto google = LinuxTranslationClient::prepare(QStringLiteral("Hello"), googleConfig);
        QVERIFY(google.error.isEmpty());
        QCOMPARE(google.request.url().toString(), QStringLiteral("https://translation.googleapis.com/language/translate/v2"));
        QCOMPARE(google.request.rawHeader("X-goog-api-key"), QByteArray("google-key"));
        QCOMPARE(QJsonDocument::fromJson(google.body).object().value(QStringLiteral("target")).toString(), QStringLiteral("zh-CN"));
        QVERIFY(!google.request.url().toString().contains(QStringLiteral("google-key")));
        LinuxTranslationConfig youdaoConfig{QStringLiteral("youdao"), {}, {}, QStringLiteral("zh"), {}, QStringLiteral("app-id"), QStringLiteral("app-secret")};
        const auto youdao = LinuxTranslationClient::prepare(QStringLiteral("Hello"), youdaoConfig);
        QVERIFY(youdao.error.isEmpty());
        QCOMPARE(youdao.request.url().toString(), QStringLiteral("https://openapi.youdao.com/api"));
        const QUrlQuery form(QString::fromUtf8(youdao.body));
        QCOMPARE(form.queryItemValue(QStringLiteral("q")), QStringLiteral("Hello"));
        QCOMPARE(form.queryItemValue(QStringLiteral("to")), QStringLiteral("zh-CHS"));
        QCOMPARE(form.queryItemValue(QStringLiteral("signType")), QStringLiteral("v3"));
        const QString raw = QStringLiteral("app-idHello") + form.queryItemValue(QStringLiteral("salt"))
            + form.queryItemValue(QStringLiteral("curtime")) + QStringLiteral("app-secret");
        QCOMPARE(form.queryItemValue(QStringLiteral("sign")), QString::fromLatin1(QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256).toHex()));
        QVERIFY(!youdao.body.contains("app-secret"));
        QVERIFY(!LinuxTranslationClient::prepare(QStringLiteral("Hello"), {QStringLiteral("youdao"), {}, {}, QStringLiteral("zh")}).error.isEmpty());
        const QString longText = QStringLiteral("1234567890123456789012345");
        const auto longYoudao = LinuxTranslationClient::prepare(longText, youdaoConfig);
        QVERIFY(longYoudao.error.isEmpty());
        const QUrlQuery longForm(QString::fromUtf8(longYoudao.body));
        const QString signedInput = longText.left(10) + QString::number(longText.size()) + longText.right(10);
        const QString longRaw = QStringLiteral("app-id") + signedInput + longForm.queryItemValue(QStringLiteral("salt"))
            + longForm.queryItemValue(QStringLiteral("curtime")) + QStringLiteral("app-secret");
        QCOMPARE(longForm.queryItemValue(QStringLiteral("sign")), QString::fromLatin1(QCryptographicHash::hash(longRaw.toUtf8(), QCryptographicHash::Sha256).toHex()));
    }
    void parsesProviderResults() {
        auto deepl = LinuxTranslationClient::parse(QStringLiteral("deepl_free"), 200,
            R"({"translations":[{"text":"你好","detected_source_language":"EN"}]})");
        QCOMPARE(deepl.text, QStringLiteral("你好")); QCOMPARE(deepl.detectedLanguage, QStringLiteral("EN"));
        auto libre = LinuxTranslationClient::parse(QStringLiteral("libretranslate"), 200,
            R"({"translatedText":"Hello","detectedLanguage":{"language":"zh"}})");
        QCOMPARE(libre.text, QStringLiteral("Hello")); QCOMPARE(libre.detectedLanguage, QStringLiteral("zh"));
        QVERIFY(!LinuxTranslationClient::parse(QStringLiteral("libretranslate"), 403, R"({"error":"Invalid API key"})").error.isEmpty());
        QVERIFY(!LinuxTranslationClient::parse(QStringLiteral("libretranslate"), 200, "not JSON").error.isEmpty());
        const auto google = LinuxTranslationClient::parse(QStringLiteral("google"), 200,
            R"({"data":{"translations":[{"translatedText":"A &amp; B","detectedSourceLanguage":"en"}]}})");
        QCOMPARE(google.text, QStringLiteral("A & B")); QCOMPARE(google.detectedLanguage, QStringLiteral("en"));
        const auto youdao = LinuxTranslationClient::parse(QStringLiteral("youdao"), 200,
            R"({"errorCode":"0","translation":["你好"],"l":"EN2zh-CHS"})");
        QCOMPARE(youdao.text, QStringLiteral("你好")); QCOMPARE(youdao.detectedLanguage, QStringLiteral("EN"));
        QVERIFY(!LinuxTranslationClient::parse(QStringLiteral("youdao"), 200, R"({"errorCode":"202"})").error.isEmpty());
    }
    void sendsConfiguredTextToLoopbackOnly() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QByteArray seen;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket, &seen] {
                seen += socket->readAll();
                const int headerEnd = seen.indexOf("\r\n\r\n");
                if (headerEnd < 0) return;
                const QByteArray headers = seen.left(headerEnd);
                int contentLength = 0;
                for (const QByteArray &line : headers.split('\n'))
                    if (line.trimmed().toLower().startsWith("content-length:"))
                        contentLength = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                if (seen.size() - headerEnd - 4 < contentLength) return;
                const QByteArray body = R"({"translatedText":"你好"})";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->flush(); socket->disconnectFromHost();
            });
        });
        LinuxTranslationClient client;
        QSignalSpy spy(&client, &LinuxTranslationClient::finished);
        const QString endpoint = QStringLiteral("http://127.0.0.1:%1/translate").arg(server.serverPort());
        client.translate(QStringLiteral("Hello"), {QStringLiteral("libretranslate"), {}, endpoint, QStringLiteral("zh")});
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(qvariant_cast<LinuxTranslationResult>(spy.first().first()).text, QStringLiteral("你好"));
        const QByteArray sentBody = seen.mid(seen.indexOf("\r\n\r\n") + 4);
        QCOMPARE(QJsonDocument::fromJson(sentBody).object().value(QStringLiteral("q")).toString(), QStringLiteral("Hello"));
    }
};

QTEST_MAIN(TranslationClientTest)
#include "translation_client_test.moc"
