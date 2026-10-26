/*
 * Strawberry Music Player
 * YouTube Music lyrics provider
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICLYRICSPROVIDER_H
#define YOUTUBEMUSICLYRICSPROVIDER_H

#include "config.h"

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QUrl>

#include "lyrics/lyricsprovider.h"
#include "core/networkaccessmanager.h"

class QNetworkReply;

// Lyrics served by YouTube Music itself: the track is looked up with InnerTube search, the
// track page yields the lyrics page id (MPLY...) and that page carries the text. No account
// and no API key of our own are needed; instrumental tracks simply have no lyrics.
class YouTubeMusicLyricsProvider : public LyricsProvider {
  Q_OBJECT

 public:
  explicit YouTubeMusicLyricsProvider(const SharedPtr<NetworkAccessManager> network, QObject *parent = nullptr);

 private:
  void StartSearch(const int id, const LyricsSearchRequest &request) override;
  void SearchReplyFinished(const int id, const LyricsSearchRequest &request, QNetworkReply *reply);
  void LyricsIdReplyFinished(const int id, const LyricsSearchRequest &request, const QString &video_id, QNetworkReply *reply);
  void LyricsReplyFinished(const int id, const LyricsSearchRequest &request, QNetworkReply *reply);

  QNetworkReply *PostInnerTube(const QString &endpoint, const QJsonObject &body);

  const SharedPtr<NetworkAccessManager> network_;

};

#endif  // YOUTUBEMUSICLYRICSPROVIDER_H
