/*
 * Strawberry Music Player
 * YouTube Music indexer logic tests
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "config.h"

#include "gtest_include.h"

#include <QHash>
#include <QList>
#include <QString>

#include "core/song.h"
#include "youtubemusic/youtubemusicindexlogic.h"

using namespace Qt::Literals::StringLiterals;
using YouTubeMusicIndexLogic::Candidate;

namespace {

constexpr qint64 kNow = 1'800'000'000;
constexpr qint64 kTtl = 7 * 24 * 60 * 60;

QList<Candidate> Candidates() {
  return {{u"a"_s, u"Astrix"_s}, {u"b"_s, u"Rezz"_s}, {u"c"_s, u"Stromae"_s}};
}

}  // namespace

TEST(YouTubeMusicIndexLogicTest, NeverIndexedArtistsAreStale) {

  const QHash<QString, qint64> stamps;
  EXPECT_TRUE(YouTubeMusicIndexLogic::IsStale(u"a"_s, stamps, kNow, kTtl));

}

TEST(YouTubeMusicIndexLogicTest, StalenessFollowsTheTtl) {

  QHash<QString, qint64> stamps;
  stamps.insert(u"fresh"_s, kNow - 60);            // a minute old
  stamps.insert(u"old"_s, kNow - kTtl - 1);        // just past the TTL
  stamps.insert(u"edge"_s, kNow - kTtl);           // exactly at the TTL

  EXPECT_FALSE(YouTubeMusicIndexLogic::IsStale(u"fresh"_s, stamps, kNow, kTtl));
  EXPECT_TRUE(YouTubeMusicIndexLogic::IsStale(u"old"_s, stamps, kNow, kTtl));
  // The boundary counts as stale: waiting one more second would be a wasted walk otherwise.
  EXPECT_TRUE(YouTubeMusicIndexLogic::IsStale(u"edge"_s, stamps, kNow, kTtl));

}

TEST(YouTubeMusicIndexLogicTest, OrdersLeastRecentlyIndexedFirst) {

  // Both stamps are past the TTL (a week), so all three artists need walking; the order is by
  // stamp, and an artist that was never indexed (stamp absent, value() == 0) comes first.
  QHash<QString, qint64> stamps;
  stamps.insert(u"a"_s, kNow - kTtl - 100);   // Astrix, indexed a week and 100s ago
  stamps.insert(u"b"_s, kNow - kTtl - 5000);  // Rezz, older
  // "c" (Stromae) was never indexed and must come first.

  const QList<Candidate> stale = YouTubeMusicIndexLogic::StaleArtists(Candidates(), stamps, kNow, kTtl);
  ASSERT_EQ(3, stale.count());
  EXPECT_EQ(u"c"_s, stale.at(0).id);
  EXPECT_EQ(u"b"_s, stale.at(1).id);
  EXPECT_EQ(u"a"_s, stale.at(2).id);

}

TEST(YouTubeMusicIndexLogicTest, SkipsArtistsWithinTheTtlAndHonoursTheCap) {

  QHash<QString, qint64> stamps;
  stamps.insert(u"a"_s, kNow - 10);
  stamps.insert(u"b"_s, kNow - 20);
  // "c" has no stamp.

  QList<Candidate> stale = YouTubeMusicIndexLogic::StaleArtists(Candidates(), stamps, kNow, kTtl);
  ASSERT_EQ(1, stale.count());
  EXPECT_EQ(u"c"_s, stale.at(0).id);

  // A cap of two with three stale artists must return the two oldest.
  const QHash<QString, qint64> no_stamps;
  stale = YouTubeMusicIndexLogic::StaleArtists(Candidates(), no_stamps, kNow, kTtl, 2);
  EXPECT_EQ(2, stale.count());

}

TEST(YouTubeMusicIndexLogicTest, AlbumContextFillsAlbumAndUsesTheWalkedArtist) {

  Song album;
  album.set_album(u"Ziran"_s);
  album.set_album_id(u"MPREb_test"_s);
  // The album's own credit line is a collaboration - it must not become the artist.
  album.set_artist(u"Rezz & PVRIS"_s);

  Song track;
  track.set_title(u"Underbeat"_s);

  SongList tracks{track};
  YouTubeMusicIndexLogic::ApplyAlbumContext(tracks, album, u"Rezz"_s);

  ASSERT_EQ(1, tracks.count());
  EXPECT_EQ(u"Ziran"_s, tracks.first().album());
  EXPECT_EQ(u"MPREb_test"_s, tracks.first().album_id());
  EXPECT_EQ(u"Rezz"_s, tracks.first().artist());
  EXPECT_EQ(u"Rezz"_s, tracks.first().albumartist());

}

TEST(YouTubeMusicIndexLogicTest, AlbumContextKeepsWhatTheTrackAlreadyHas) {

  Song album;
  album.set_album(u"Album from the row"_s);
  album.set_album_id(u"MPREb_row"_s);

  Song track;
  track.set_album(u"Album from the page"_s);
  track.set_album_id(u"MPREb_page"_s);

  SongList tracks{track};
  YouTubeMusicIndexLogic::ApplyAlbumContext(tracks, album, u"Astrix"_s);

  EXPECT_EQ(u"Album from the page"_s, tracks.first().album());
  EXPECT_EQ(u"MPREb_page"_s, tracks.first().album_id());

}

TEST(YouTubeMusicIndexLogicTest, AlbumContextFallsBackToTheCreditWhenNoArtistWasWalked) {

  Song album;
  album.set_artist(u"Stromae"_s);

  Song track;
  SongList tracks{track};
  YouTubeMusicIndexLogic::ApplyAlbumContext(tracks, album, QString());

  EXPECT_EQ(u"Stromae"_s, tracks.first().artist());

}
