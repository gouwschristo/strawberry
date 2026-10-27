/*
 * Strawberry Music Player
 * YouTube Music parser tests
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "config.h"

#include "gtest_include.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "youtubemusic/youtubemusicparser.h"

using namespace Qt::Literals::StringLiterals;

namespace {

QJsonObject Parse(const char *json) {
  return QJsonDocument::fromJson(QByteArray(json)).object();
}

}  // namespace

// A row served in the "playlist" shape carries its id only in playlistItemData, with a null
// navigationEndpoint. Ignoring that left whole singles and community albums without tracks.
TEST(YouTubeMusicParserTest, ReadsVideoIdFromPlaylistShapeRows) {

  const QJsonObject row = Parse(R"({
    "musicResponsiveListItemRenderer": {
      "playlistItemData": {"videoId": "RBBXKqfZ7oA"},
      "navigationEndpoint": null
    }
  })");

  EXPECT_EQ(u"RBBXKqfZ7oA"_s, YouTubeMusicParser::VideoId(row));

}

TEST(YouTubeMusicParserTest, ReadsVideoIdFromWatchEndpointRows) {

  const QJsonObject row = Parse(R"({
    "musicResponsiveListItemRenderer": {
      "overlay": {
        "musicItemThumbnailOverlayRenderer": {
          "content": {
            "musicPlayButtonRenderer": {
              "playNavigationEndpoint": {"watchEndpoint": {"videoId": "H7_rn3Aq1qc"}}
            }
          }
        }
      }
    }
  })");

  EXPECT_EQ(u"H7_rn3Aq1qc"_s, YouTubeMusicParser::VideoId(row));

}

TEST(YouTubeMusicParserTest, ReturnsNoVideoIdWhenTheRowHasNone) {

  const QJsonObject header = Parse(R"({"musicResponsiveHeaderRenderer": {"title": {"runs": []}}})");
  EXPECT_TRUE(YouTubeMusicParser::VideoId(header).isEmpty());

}

TEST(YouTubeMusicParserTest, ReadsTheReleaseYearFromTheAlbumHeader) {

  const QJsonObject album = Parse(R"({"header": {"musicResponsiveHeaderRenderer": {
    "subtitle": {"runs": [{"text": "Album"}, {"text": " \u2022 "}, {"text": "2004"}]}
  }}})");
  EXPECT_EQ(2004, YouTubeMusicParser::AlbumYearFromHeader(album));

}

TEST(YouTubeMusicParserTest, DoesNotMistakeALabelForAYear) {

  // "Album - Alpha Portal": the slot that usually carries the year holds the label instead.
  const QJsonObject album = Parse(R"({"header": {"musicResponsiveHeaderRenderer": {
    "subtitle": {"runs": [{"text": "Album"}, {"text": " \u2022 "}, {"text": "Alpha Portal"}]}
  }}})");
  EXPECT_EQ(-1, YouTubeMusicParser::AlbumYearFromHeader(album));

  // A single carries "Single - 2024", which must still be read.
  const QJsonObject single = Parse(R"({"header": {"musicResponsiveHeaderRenderer": {
    "subtitle": {"runs": [{"text": "Single"}, {"text": " \u2022 "}, {"text": "2024"}]}
  }}})");
  EXPECT_EQ(2024, YouTubeMusicParser::AlbumYearFromHeader(single));

}

TEST(YouTubeMusicParserTest, CollectsBothRowShapesAndCanRestrictToLists) {

  const QJsonObject reply = Parse(R"({
    "contents": {"singleColumnBrowseResultsRenderer": {"tabs": [{"tabRenderer": {"content": {
      "sectionListRenderer": {"contents": [
        {"musicResponsiveListItemRenderer": {"item": "list"}},
        {"musicTwoRowItemRenderer": {"item": "grid"}},
        {"nested": {"deeper": [{"musicResponsiveListItemRenderer": {"item": "list2"}}]}}
      ]}
    }}}]}}
  })");

  QList<QJsonObject> all;
  YouTubeMusicParser::CollectItems(reply, all);
  EXPECT_EQ(3, all.count());

  QList<QJsonObject> list_rows_only;
  YouTubeMusicParser::CollectItems(reply, list_rows_only, true);
  EXPECT_EQ(2, list_rows_only.count());

}

TEST(YouTubeMusicParserTest, ReadsTextObjects) {

  const QJsonObject simple = Parse(R"({"simpleText": "Astrix"})");
  EXPECT_EQ((QStringList{u"Astrix"_s}), YouTubeMusicParser::TextRuns(simple));

  const QJsonObject runs = Parse(R"({"runs": [{"text": "Album"}, {"text": " - "}, {"text": "2004"}]})");
  EXPECT_EQ((QStringList{u"Album"_s, u" - "_s, u"2004"_s}), YouTubeMusicParser::TextRuns(runs));

  EXPECT_TRUE(YouTubeMusicParser::TextRuns(QJsonValue()).isEmpty());

}
