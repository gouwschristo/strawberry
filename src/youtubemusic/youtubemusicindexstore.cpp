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
#include "youtubemusicindexstore.h"

#include <QHash>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include "core/database.h"
#include "core/logging.h"
#include "core/sqlquery.h"

using namespace Qt::Literals::StringLiterals;

namespace YouTubeMusicStore {

namespace {
// Every query here runs on the database's connection and reports errors the same way.
QSqlDatabase Connect(const SharedPtr<Database> &db) {
  return db->Connect();
}
}  // namespace

QHash<QString, qint64> ReadStamps(const SharedPtr<Database> &db) {

  QHash<QString, qint64> stamps;

  QMutexLocker locker(db->Mutex());
  QSqlDatabase connection = Connect(db);
  SqlQuery query(connection);
  query.prepare(QStringLiteral("SELECT artist_id, indexed_at FROM youtubemusic_index"));
  if (!query.Exec()) {
    db->ReportErrors(query);
    return stamps;
  }

  while (query.next()) stamps.insert(query.value(0).toString(), query.value(1).toLongLong());

  return stamps;

}

bool WriteStamp(const SharedPtr<Database> &db, const QString &artist_id, const qint64 indexed_at) {

  QMutexLocker locker(db->Mutex());
  SqlQuery query(Connect(db));
  query.prepare(QStringLiteral("INSERT OR REPLACE INTO youtubemusic_index (artist_id, indexed_at) VALUES (?, ?)"));
  query.addBindValue(artist_id);
  query.addBindValue(indexed_at);
  if (!query.Exec()) {
    db->ReportErrors(query);
    return false;
  }

  return true;

}

QStringList TreeNames(const SharedPtr<Database> &db) {

  QStringList names;

  QMutexLocker locker(db->Mutex());
  SqlQuery query(Connect(db));
  query.prepare(QStringLiteral("SELECT DISTINCT artist FROM youtubemusic_artists_songs WHERE artist <> ''"));
  if (!query.Exec()) {
    db->ReportErrors(query);
    return names;
  }

  while (query.next()) names << query.value(0).toString();

  return names;

}

QStringList SubscribedNames(const SharedPtr<Database> &db) {

  QStringList names;

  QMutexLocker locker(db->Mutex());
  SqlQuery query(Connect(db));
  query.prepare(QStringLiteral("SELECT DISTINCT artist FROM youtubemusic_artists_songs "
                               "WHERE coalesce(album_id, '') = '' AND coalesce(song_id, '') = '' AND artist <> ''"));
  if (!query.Exec()) {
    db->ReportErrors(query);
    return names;
  }

  while (query.next()) names << query.value(0).toString();

  return names;

}

int RemoveArtist(const SharedPtr<Database> &db, const QString &artist) {

  QMutexLocker locker(db->Mutex());
  SqlQuery query(Connect(db));
  query.prepare(QStringLiteral("DELETE FROM youtubemusic_artists_songs WHERE artist = ?"));
  query.addBindValue(artist);
  if (!query.Exec()) {
    db->ReportErrors(query);
    return -1;
  }

  return query.numRowsAffected();

}

int RemoveAlbumsWithoutTracks(const SharedPtr<Database> &db) {

  QMutexLocker locker(db->Mutex());
  SqlQuery query(Connect(db));
  query.prepare(QStringLiteral("DELETE FROM youtubemusic_artists_songs WHERE album_id <> '' AND album_id IN ("
                               "SELECT album_id FROM youtubemusic_artists_songs WHERE album_id <> '' "
                               "GROUP BY album_id HAVING sum(CASE WHEN coalesce(song_id, '') <> '' THEN 1 ELSE 0 END) = 0)"));
  if (!query.Exec()) {
    db->ReportErrors(query);
    return -1;
  }

  return query.numRowsAffected();

}

}  // namespace YouTubeMusicStore
