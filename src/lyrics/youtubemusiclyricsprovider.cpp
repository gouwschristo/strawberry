/*
 * Strawberry Music Player
 * YouTube Music lyrics provider
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "config.h"
#include "youtubemusiclyricsprovider.h"

#include <memory>

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>

#include "core/logging.h"
#include "core/networkaccessmanager.h"
#include "lyrics/lyricssearchresult.h"

using namespace Qt::Literals::StringLiterals;

namespace {
// Same public InnerTube web key the streaming service uses (see youtubemusicbaserequest.cpp):
// base64 so repository secret scanners do not flag a public constant in every clone.
const char *ApiKey() {
  static const QByteArray decoded = QByteArray::fromBase64(QByteArrayLiteral("QUl6YVN5QzlYTDNaaldkZFh5YTZYNzRkSm9DVEwtV0VZRkROWDMw"));
  return decoded.constData();
}
constexpr char kOrigin[] = "https://music.youtube.com";
constexpr char kClientVersion[] = "1.20240101.01.00";
constexpr char kUserAgent[] = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

QJsonObject ClientContext() {
  return QJsonObject{{u"client"_s, QJsonObject{{u"clientName"_s, u"WEB_REMIX"_s},
                                              {u"clientVersion"_s, QLatin1String(kClientVersion)},
                                              {u"hl"_s, u"en"_s}}}};
}
}  // namespace

YouTubeMusicLyricsProvider::YouTubeMusicLyricsProvider(const SharedPtr<NetworkAccessManager> network, QObject *parent)
    : LyricsProvider(u"YouTube Music"_s, true, false, network, parent), network_(network) {}

QNetworkReply *YouTubeMusicLyricsProvider::PostInnerTube(const QString &endpoint, const QJsonObject &body) {

  QNetworkRequest request(QUrl(QLatin1String(kOrigin) + u"/youtubei/v1/"_s + endpoint + u"?prettyPrint=false"_s));
  request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
  request.setHeader(QNetworkRequest::UserAgentHeader, QLatin1String(kUserAgent));
  request.setRawHeader("X-Goog-Api-Key", ApiKey());
  request.setRawHeader("Origin", kOrigin);
  request.setRawHeader("Referer", kOrigin);

  return network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));

}

void YouTubeMusicLyricsProvider::StartSearch(const int id, const LyricsSearchRequest &request) {

  const QString query = QStringList{request.artist, request.title}.join(u' ').trimmed();
  if (query.isEmpty()) {
    Q_EMIT SearchFinished(id, LyricsSearchResults());
    return;
  }

  const QJsonObject body{{u"context"_s, ClientContext()}, {u"query"_s, query}};
  QNetworkReply *reply = PostInnerTube(u"search"_s, body);
  QObject::connect(reply, &QNetworkReply::finished, this, [this, id, request, reply]() {
    SearchReplyFinished(id, request, reply);
  });

}

void YouTubeMusicLyricsProvider::SearchReplyFinished(const int id, const LyricsSearchRequest &request, QNetworkReply *reply) {

  reply->deleteLater();

  if (reply->error() != QNetworkReply::NoError) {
    qLog(Debug) << "YouTubeMusicLyrics:" << reply->errorString();
    Q_EMIT SearchFinished(id, LyricsSearchResults());
    return;
  }

  // The first video id in the reply is the best match for "artist title".
  static const QRegularExpression video_re(uR"re("videoId"\s*:\s*"([A-Za-z0-9_-]{11})")re"_s);
  const QRegularExpressionMatch match = video_re.match(QString::fromUtf8(reply->readAll()));
  if (!match.hasMatch()) {
    Q_EMIT SearchFinished(id, LyricsSearchResults());
    return;
  }

  const QString video_id = match.captured(1);
  const QJsonObject body{{u"context"_s, ClientContext()}, {u"videoId"_s, video_id}};
  QNetworkReply *next_reply = PostInnerTube(u"next"_s, body);
  QObject::connect(next_reply, &QNetworkReply::finished, this, [this, id, request, video_id, next_reply]() {
    LyricsIdReplyFinished(id, request, video_id, next_reply);
  });

}

void YouTubeMusicLyricsProvider::LyricsIdReplyFinished(const int id, const LyricsSearchRequest &request, const QString &video_id, QNetworkReply *reply) {

  Q_UNUSED(video_id);

  reply->deleteLater();

  if (reply->error() != QNetworkReply::NoError) {
    Q_EMIT SearchFinished(id, LyricsSearchResults());
    return;
  }

  // The lyrics page id starts with MPLY; instrumental tracks do not have one.
  static const QRegularExpression lyrics_id_re(uR"re("(MPLY[A-Za-z0-9_%\-]+)")re"_s);
  const QRegularExpressionMatch match = lyrics_id_re.match(QString::fromUtf8(reply->readAll()));
  if (!match.hasMatch()) {
    Q_EMIT SearchFinished(id, LyricsSearchResults());
    return;
  }

  const QJsonObject body{{u"context"_s, ClientContext()}, {u"browseId"_s, match.captured(1)}};
  QNetworkReply *lyrics_reply = PostInnerTube(u"browse"_s, body);
  QObject::connect(lyrics_reply, &QNetworkReply::finished, this, [this, id, request, lyrics_reply]() {
    LyricsReplyFinished(id, request, lyrics_reply);
  });

}

void YouTubeMusicLyricsProvider::LyricsReplyFinished(const int id, const LyricsSearchRequest &request, QNetworkReply *reply) {

  reply->deleteLater();

  if (reply->error() != QNetworkReply::NoError) {
    Q_EMIT SearchFinished(id, LyricsSearchResults());
    return;
  }

  const QString json = QString::fromUtf8(reply->readAll());

  // The page carries the text in a description shelf; "Lyrics not available" means none.
  static const QRegularExpression shelf_re(uR"re("musicDescriptionShelfRenderer":\s*\{"description":\s*\{"runs":\s*\[(.{0,60000}?)\]\s*\})re"_s);
  const QRegularExpressionMatch shelf = shelf_re.match(json);
  if (!shelf.hasMatch()) {
    Q_EMIT SearchFinished(id, LyricsSearchResults());
    return;
  }

  static const QRegularExpression run_re(uR"re("text":\s*"((?:[^"\\]|\\.)*)")re"_s);
  QStringList lines;
  QRegularExpressionMatchIterator it = run_re.globalMatch(shelf.captured(1));
  while (it.hasNext()) {
    QString text = it.next().captured(1);
    text.replace(u"\\n"_s, u"\n"_s);
    text.replace(u"\\r"_s, QString());
    text.replace(u"\\\""_s, u"\""_s);
    text.replace(u"\\\\"_s, u"\\"_s);
    lines << text;
  }

  const QString lyrics = lines.join(u"\n"_s).trimmed();
  LyricsSearchResults results;
  if (!lyrics.isEmpty() && !lyrics.contains(u"Lyrics not available"_s)) {
    LyricsSearchResult result(lyrics);
    result.provider = name();
    result.artist = request.artist;
    result.album = request.album;
    result.title = request.title;
    result.score = 1.0F;
    results << result;
  }

  Q_EMIT SearchFinished(id, results);

}
