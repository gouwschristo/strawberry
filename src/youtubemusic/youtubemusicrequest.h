/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICREQUEST_H
#define YOUTUBEMUSICREQUEST_H

#include "config.h"

#include <QString>
#include <QJsonObject>

#include "includes/shared_ptr.h"
#include "core/song.h"
#include "youtubemusicbaserequest.h"

class NetworkAccessManager;
class YouTubeMusicService;

class YouTubeMusicRequest : public YouTubeMusicBaseRequest {
  Q_OBJECT

 public:
  enum class Type {
    None,
    SearchArtists,
    SearchAlbums,
    SearchSongs,
    LibraryArtists,
    LibraryAlbums,
    LibrarySongs,
    AlbumSongs,
    ArtistSongs,
    ArtistAlbums
  };

  explicit YouTubeMusicRequest(YouTubeMusicService *service, const SharedPtr<NetworkAccessManager> network, QObject *parent = nullptr);

  void Process(const Type type, const int search_id = 0, const QString &query = QString());
  // "Load more": let this request follow more continuation pages than the default.
  void set_page_budget(const int pages) { page_budget_ = pages; }
  void ProcessAlbum(const Song &album);
  void ProcessArtist(const Song &artist);
  void ProcessArtistAlbums(const Song &artist);

 Q_SIGNALS:
  void Results(const int id, const SongMap &songs, const QString &error);
  // Rows of one intermediate page, so a paginated list fills while it loads. Emitted from
  // inside the request, so consumers must not delete the request on it.
  void PageReady(const int id, const SongMap &songs);
  void UpdateStatus(const QString &text);

 private Q_SLOTS:
  void ReplyFinished();

 private:
  void ParseJson(const QJsonObject &json_object);
  void RequestContinuation(const QString &token);
  void Send(const QString &endpoint, const QJsonObject &body);
  QString FindContinuation(const QJsonObject &json_object) const;
  void EmitResults();
  // Emits only the rows parsed since the last emit, so a paginated list fills while it loads
  // instead of appearing all at once when the last page arrives.
  void EmitNewResults();
  QSet<QString> emitted_keys_;
  QString ParseTitle(const QJsonObject &item) const;
  Song ParseSongItem(const QJsonObject &item) const;
  Song ParseArtistItem(const QJsonObject &item) const;
  Song ParseAlbumItem(const QJsonObject &item, const bool strict_page_type = false) const;

  Type type_;
  int search_id_;
  QString query_;
  QString error_;
  SongMap songs_;
  int pages_;
  int page_budget_ = 0;   // 0 = use the built-in caps
  QJsonObject last_body_;
  QString last_endpoint_;
  bool retried_auth_;
  Song album_source_;
  Song artist_source_;
  QString resolved_artist_browse_id_;
};

#endif  // YOUTUBEMUSICREQUEST_H
