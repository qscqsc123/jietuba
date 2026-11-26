#include "translation_client.h"
#include "core/html_unescape.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

namespace {
QString youdaoTarget(const QString &code) { return code == QLatin1String("zh") ? QStringLiteral("zh-CHS") : code; }
QString googleTarget(const QString &code) { return code == QLatin1String("zh") ? QStringLiteral("zh-CN") : code; }

// 有道 v3：超过 20 个 UTF-16 单元时取首尾各 10 个和长度，与原版签名路径一致。
// Youdao v3: use the first and last ten UTF-16 units plus length for long input.
QString youdaoInput(const QString &text) {
    return text.size() <= 20 ? text : text.left(10) + QString::number(text.size()) + text.right(10);
}
}

LinuxTranslationClient::LinuxTranslationClient(QObject *parent) : QObject(parent) {}

LinuxTranslationPreparation LinuxTranslationClient::prepare(const QString &text, const LinuxTranslationConfig &config) {
    LinuxTranslationPreparation prepared;
    if (text.trimmed().isEmpty()) { prepared.error = QStringLiteral("没有可翻译的文字。"); return prepared; }
    const QString provider = config.provider;
    QUrl url;
    QJsonObject body;
    if (provider == QLatin1String("deepl_free") || provider == QLatin1String("deepl_pro")) {
        if (config.apiKey.trimmed().isEmpty()) {
            prepared.error = QStringLiteral("请在设置 → 翻译中填写 DeepL API 密钥。"); return prepared;
        }
        url = QUrl(provider == QLatin1String("deepl_pro") ? QStringLiteral("https://api.deepl.com/v2/translate")
                                                       : QStringLiteral("https://api-free.deepl.com/v2/translate"));
        body = {{QStringLiteral("text"), QJsonArray{text}}, {QStringLiteral("target_lang"), config.target.toUpper()}};
        // API 密钥只放在请求头，不出现在 URL、错误提示或日志中。
        // Keep the credential in the Authorization header, away from URLs and logs.
        prepared.request.setRawHeader("Authorization", "DeepL-Auth-Key " + config.apiKey.trimmed().toUtf8());
    } else if (provider == QLatin1String("libretranslate")) {
        url = QUrl(config.endpoint.trimmed(), QUrl::StrictMode);
        if (!url.isValid() || (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http")) || url.host().isEmpty()) {
            prepared.error = QStringLiteral("请在设置 → 翻译中填写有效的 LibreTranslate /translate 地址。"); return prepared;
        }
        body = {{QStringLiteral("q"), text}, {QStringLiteral("source"), QStringLiteral("auto")},
                {QStringLiteral("target"), config.target}};
        if (!config.apiKey.trimmed().isEmpty()) body.insert(QStringLiteral("api_key"), config.apiKey.trimmed());
    } else if (provider == QLatin1String("google")) {
        if (config.googleApiKey.trimmed().isEmpty()) {
            prepared.error = QStringLiteral("请在设置 → 翻译中填写 Google API Key。"); return prepared;
        }
        url = QUrl(QStringLiteral("https://translation.googleapis.com/language/translate/v2"));
        body = {{QStringLiteral("q"), text}, {QStringLiteral("target"), googleTarget(config.target)},
                {QStringLiteral("format"), QStringLiteral("text")}};
        // 官方 REST 接口允许 x-goog-api-key 请求头，避免把密钥放进 URL。
        // Google's REST API accepts this header, avoiding a credential-bearing URL.
        prepared.request.setRawHeader("X-goog-api-key", config.googleApiKey.trimmed().toUtf8());
    } else if (provider == QLatin1String("youdao")) {
        const QString appId = config.youdaoAppId.trimmed();
        const QString secret = config.youdaoAppSecret.trimmed();
        if (appId.isEmpty() || secret.isEmpty()) {
            prepared.error = QStringLiteral("请在设置 → 翻译中填写有道应用 ID 和应用密钥。"); return prepared;
        }
        if (text.toUcs4().size() > 5000) {
            prepared.error = QStringLiteral("有道单次翻译最多支持 5000 个字符，请缩小选区。"); return prepared;
        }
        url = QUrl(QStringLiteral("https://openapi.youdao.com/api"));
        const QString salt = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString curtime = QString::number(QDateTime::currentSecsSinceEpoch());
        const QString raw = appId + youdaoInput(text) + salt + curtime + secret;
        const QString sign = QString::fromLatin1(QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256).toHex());
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("q"), text);
        form.addQueryItem(QStringLiteral("from"), QStringLiteral("auto"));
        form.addQueryItem(QStringLiteral("to"), youdaoTarget(config.target));
        form.addQueryItem(QStringLiteral("appKey"), appId);
        form.addQueryItem(QStringLiteral("salt"), salt);
        form.addQueryItem(QStringLiteral("sign"), sign);
        form.addQueryItem(QStringLiteral("signType"), QStringLiteral("v3"));
        form.addQueryItem(QStringLiteral("curtime"), curtime);
        prepared.body = form.query(QUrl::FullyEncoded).toUtf8();
        prepared.request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded; charset=UTF-8"));
    } else {
        prepared.error = QStringLiteral("不支持的翻译服务。"); return prepared;
    }
    prepared.request.setUrl(url);
    if (provider != QLatin1String("youdao")) {
        prepared.request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        prepared.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    }
    prepared.request.setTransferTimeout(15000);
    return prepared;
}

LinuxTranslationResult LinuxTranslationClient::parse(const QString &provider, int httpStatus, const QByteArray &response) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    const QJsonObject json = document.object();
    if (httpStatus >= 400) {
        const QJsonValue errorValue = json.value(QStringLiteral("error"));
        const QString detail = json.value(QStringLiteral("message")).toString(
            errorValue.isObject() ? errorValue.toObject().value(QStringLiteral("message")).toString() : errorValue.toString());
        return {{}, {}, detail.isEmpty() ? QStringLiteral("翻译服务返回 HTTP %1。").arg(httpStatus)
                                       : QStringLiteral("翻译服务返回 HTTP %1：%2").arg(httpStatus).arg(detail)};
    }
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return {{}, {}, QStringLiteral("翻译服务返回了无效的 JSON。")};
    if (provider == QLatin1String("libretranslate")) {
        const QString translated = json.value(QStringLiteral("translatedText")).toString();
        if (translated.isEmpty()) return {{}, {}, QStringLiteral("翻译服务没有返回译文。")};
        return {translated, json.value(QStringLiteral("detectedLanguage")).toObject().value(QStringLiteral("language")).toString(), {}};
    }
    if (provider == QLatin1String("google")) {
        const QJsonArray translations = json.value(QStringLiteral("data")).toObject().value(QStringLiteral("translations")).toArray();
        const QJsonObject first = translations.isEmpty() ? QJsonObject{} : translations.first().toObject();
        const QString translated = jietuba::html_unescape(first.value(QStringLiteral("translatedText")).toString());
        if (translated.isEmpty()) return {{}, {}, QStringLiteral("Google 没有返回译文。")};
        return {translated, first.value(QStringLiteral("detectedSourceLanguage")).toString(), {}};
    }
    if (provider == QLatin1String("youdao")) {
        const QString code = json.value(QStringLiteral("errorCode")).toVariant().toString();
        if (code != QLatin1String("0")) {
            const QString reason = code == QLatin1String("202") ? QStringLiteral("签名验证失败，请检查应用 ID 和密钥。")
                : code == QLatin1String("108") ? QStringLiteral("应用 ID 无效。")
                : code == QLatin1String("110") ? QStringLiteral("该有道应用尚未开通文本翻译。")
                : code == QLatin1String("206") ? QStringLiteral("时间戳无效，请检查系统时间。")
                : QStringLiteral("有道返回错误码 %1。").arg(code.isEmpty() ? QStringLiteral("未知") : code);
            return {{}, {}, reason};
        }
        QStringList lines;
        for (const auto &item : json.value(QStringLiteral("translation")).toArray()) lines.append(item.toString());
        const QString translated = lines.join(QLatin1Char('\n'));
        if (translated.trimmed().isEmpty()) return {{}, {}, QStringLiteral("有道没有返回译文。")};
        return {translated, json.value(QStringLiteral("l")).toString().section(QLatin1Char('2'), 0, 0), {}};
    }
    const QJsonArray translations = json.value(QStringLiteral("translations")).toArray();
    const QJsonObject first = translations.isEmpty() ? QJsonObject{} : translations.first().toObject();
    const QString translated = first.value(QStringLiteral("text")).toString();
    if (translated.isEmpty()) return {{}, {}, QStringLiteral("DeepL 没有返回译文。")};
    return {translated, first.value(QStringLiteral("detected_source_language")).toString(), {}};
}

void LinuxTranslationClient::translate(const QString &text, const LinuxTranslationConfig &config) {
    const auto prepared = prepare(text, config);
    if (!prepared.error.isEmpty()) { emit finished({{}, {}, prepared.error}); return; }
    if (current_) current_->abort();
    auto *reply = manager_.post(prepared.request, prepared.body);
    current_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, provider = config.provider] {
        if (reply != current_) { reply->deleteLater(); return; }
        current_.clear();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray response = reply->read(1024 * 1024 + 1);
        LinuxTranslationResult result;
        if (response.size() > 1024 * 1024) result.error = QStringLiteral("翻译服务响应过大。");
        else if (reply->error() != QNetworkReply::NoError && status == 0) result.error = QStringLiteral("翻译网络错误：%1").arg(reply->errorString());
        else result = parse(provider, status, response);
        reply->deleteLater();
        emit finished(result);
    });
}
