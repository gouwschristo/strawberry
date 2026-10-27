/*
 * Strawberry Music Player
 * YouTube Music store tests
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "config.h"

#include "gtest_include.h"

#include <memory>

#include <QHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>

#include "core/database.h"
#include "core/memorydatabase.h"
#include "core/sqlquery.h"
#include "includes/shared_ptr.h"
#include "youtubemusic/youtubemusicindexstore.h"

using namespace Qt::Literals::StringLiterals;
using std::make_shared;

namespace {

class YouTubeMusicStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    database_ = make_shared<MemoryDatabase>(nullptr);

    // MemoryDatabase applies the application schema, which already contains the
    // youtubemusic_* tables (schema 25), so the test runs against the real definitions.
  }

  // Inserts one tree row. An empty song_id means "album row", an empty album_id "artist row".
  void Insert(const QString &artist, const QString &album = QString(), const QString &song_id = QString(),
              const QString &album_id = QString(), const QString &artist_id = QString()) {
    QMutexLocker locker(database_->Mutex());
    SqlQuery query(database_->Connect());
    query.prepare(QStringLiteral("INSERT INTO youtubemusic_artists_songs (url, artist, album, song_id, album_id, artist_id) "
                                 "VALUES (?, ?, ?, ?, ?, ?)"));
    query.addBindValue(QString(u"youtubemusic://test/%1/%2"_s).arg(artist).arg(++row_));
    query.addBindValue(artist);
    query.addBindValue(album);
    query.addBindValue(song_id.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(song_id));
    query.addBindValue(album_id.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(album_id));
    query.addBindValue(artist_id.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(artist_id));
    ASSERT_TRUE(query.Exec());
  }

  int CountRows() {
    QMutexLocker locker(database_->Mutex());
    SqlQuery query(database_->Connect());
    query.prepare(QStringLiteral("SELECT count(*) FROM youtubemusic_artists_songs"));
    if (!query.Exec() || !query.next()) return -1;
    return query.value(0).toInt();
  }

  SharedPtr<Database> database_;
  int row_ = 0;
};

}  // namespace

TEST_F(YouTubeMusicStoreTest, SubscriptionListIsTheShelfNotTheSongs) {

  // The liked-artists shelf: rows with neither an album nor a song.
  Insert(u"Rezz"_s, QString(), QString(), QString(), u"MPLAUC1"_s);
  Insert(u"Stromae"_s, QString(), QString(), QString(), u"MPLAUC2"_s);
  // Rows that came from walking/serving an album: an artist name appears, but it is not followed.
  Insert(u"Rezz"_s, u"Ziran"_s, u"H7_rn3Aq1qc"_s, u"MPREb_1"_s, u"MPLAUC1"_s);
  Insert(u"Samantha Machado"_s, u"Album"_s, u"v1Cm1GTaRCI"_s, u"MPREb_2"_s, u"MPLAUC9"_s);

  const QStringList subscribed = YouTubeMusicStore::SubscribedNames(database_);
  EXPECT_EQ((QStringList{u"Rezz"_s, u"Stromae"_s}), subscribed);

  const QStringList tree = YouTubeMusicStore::TreeNames(database_);
  ASSERT_EQ(3, tree.count());
  EXPECT_TRUE(tree.contains(u"Samantha Machado"_s));

}

TEST_F(YouTubeMusicStoreTest, RemovingAnArtistRemovesAllItsRowsAndNothingElse) {

  Insert(u"Rezz"_s, QString(), QString(), QString(), u"MPLAUC1"_s);
  Insert(u"Rezz"_s, u"Ziran"_s, u"H7_rn3Aq1qc"_s, u"MPREb_1"_s, u"MPLAUC1"_s);
  Insert(u"Stromae"_s, u"Cheese"_s, u"abc"_s, u"MPREb_3"_s, u"MPLAUC2"_s);
  // A name that merely starts with the one being removed: a LIKE-style delete would take it too.
  Insert(u"Rezz & PVRIS"_s, u"Album"_s, u"def"_s, u"MPREb_4"_s, u"MPLAUC4"_s);

  EXPECT_EQ(2, YouTubeMusicStore::RemoveArtist(database_, u"Rezz"_s));
  EXPECT_EQ(2, CountRows());

  const QStringList tree = YouTubeMusicStore::TreeNames(database_);
  ASSERT_EQ(2, tree.count());
  EXPECT_TRUE(tree.contains(u"Stromae"_s));
  EXPECT_TRUE(tree.contains(u"Rezz & PVRIS"_s));

}

TEST_F(YouTubeMusicStoreTest, StampsRoundTripAndOverwrite) {

  EXPECT_TRUE(YouTubeMusicStore::ReadStamps(database_).isEmpty());

  EXPECT_TRUE(YouTubeMusicStore::WriteStamp(database_, u"MPLAUC1"_s, 1700000000));
  EXPECT_TRUE(YouTubeMusicStore::WriteStamp(database_, u"MPLAUC2"_s, 1700000100));
  // Writing again replaces the stamp instead of adding a row (INSERT OR REPLACE).
  EXPECT_TRUE(YouTubeMusicStore::WriteStamp(database_, u"MPLAUC1"_s, 1700000200));

  const QHash<QString, qint64> stamps = YouTubeMusicStore::ReadStamps(database_);
  ASSERT_EQ(2, stamps.count());
  EXPECT_EQ(1700000200, stamps.value(u"MPLAUC1"_s));
  EXPECT_EQ(1700000100, stamps.value(u"MPLAUC2"_s));

}

TEST_F(YouTubeMusicStoreTest, AlbumsWithoutTracksAreRemoved) {

  // An album row whose page never yielded a track.
  Insert(u"Stromae"_s, u"Soleil Bleu"_s, QString(), u"MPREb_empty"_s, u"MPLAUC2"_s);
  // A healthy album: descriptor row plus a track.
  Insert(u"Stromae"_s, u"Cheese"_s, QString(), u"MPREb_ok"_s, u"MPLAUC2"_s);
  Insert(u"Stromae"_s, u"Cheese"_s, u"abc"_s, u"MPREb_ok"_s, u"MPLAUC2"_s);

  EXPECT_EQ(1, YouTubeMusicStore::RemoveAlbumsWithoutTracks(database_));
  EXPECT_EQ(2, CountRows());   // the healthy album's row and its track survive

}
