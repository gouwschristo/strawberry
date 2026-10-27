/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "config.h"
#include "youtubemusicclient.h"

#include <QAnyStringView>
#include <QByteArray>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

using namespace Qt::Literals::StringLiterals;

namespace YouTubeMusicClient {

QByteArray ApiKey() {
  static const QByteArray decoded = QByteArray::fromBase64(QByteArrayLiteral("QUl6YVN5QzlYTDNaaldkZFh5YTZYNzRkSm9DVEwtV0VZRkROWDMw"));
  return decoded;
}

QString ClientVersion() {
  return u"1.20240101.01.00"_s;
}

QString UserAgent() {
  return u"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"_s;
}

QString Origin() {
  return u"https://music.youtube.com"_s;
}

QJsonObject Context() {
  return QJsonObject{{u"client"_s, QJsonObject{{u"clientName"_s, u"WEB_REMIX"_s},
                                              {u"clientVersion"_s, ClientVersion()},
                                              {u"hl"_s, u"en"_s}}}};
}

QNetworkRequest CreateRequest(const QString &endpoint) {

  QNetworkRequest request(QUrl(Origin() + endpoint));
  request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
  request.setHeader(QNetworkRequest::UserAgentHeader, UserAgent());
  request.setRawHeader("X-Goog-Api-Key", ApiKey());
  request.setRawHeader("Origin", Origin().toUtf8());
  request.setRawHeader("Referer", Origin().toUtf8());

  return request;

}

}  // namespace YouTubeMusicClient
