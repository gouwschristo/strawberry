/*
 * Strawberry Music Player
 * YouTube Music name handling tests
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "config.h"

#include "gtest_include.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include "youtubemusic/youtubemusicnameutils.h"

using namespace Qt::Literals::StringLiterals;

namespace {

QHash<QString, QString> Subscribed() {
  QHash<QString, QString> subscribed;
  // Deliberately contains a short name that is a substring of a longer one ("Berg" inside
  // "Berg & Vermont"): the longest match must win, otherwise a band gets split.
  const QStringList names = {u"Rezz"_s, u"Astrix"_s, u"Chase & Status"_s, u"Stromae"_s, u"Berg"_s, u"Berg & Vermont"_s};
  for (const QString &name : names) subscribed.insert(name.toLower(), name);
  return subscribed;
}

}  // namespace

TEST(YouTubeMusicNamesTest, RecognisesCollaborations) {

  EXPECT_TRUE(YouTubeMusicNames::LooksLikeCollaboration(u"Rezz & PVRIS"_s));
  EXPECT_TRUE(YouTubeMusicNames::LooksLikeCollaboration(u"Astrix feat. Ace Ventura"_s));
  EXPECT_TRUE(YouTubeMusicNames::LooksLikeCollaboration(u"Madvillain, Madlib & MF Doom"_s));
  EXPECT_TRUE(YouTubeMusicNames::LooksLikeCollaboration(u"CloZee x LSDREAM"_s));

  EXPECT_FALSE(YouTubeMusicNames::LooksLikeCollaboration(u"Stromae"_s));
  // The "x" separator must not fire inside a word.
  EXPECT_FALSE(YouTubeMusicNames::LooksLikeCollaboration(u"Xzibit"_s));
  EXPECT_FALSE(YouTubeMusicNames::LooksLikeCollaboration(u"Of Mice & Men"_s) == false);

}

TEST(YouTubeMusicNamesTest, SplitsCollaborations) {

  EXPECT_EQ((QStringList{u"Rezz"_s, u"PVRIS"_s}), YouTubeMusicNames::SplitCollaboration(u"Rezz & PVRIS"_s));
  EXPECT_EQ((QStringList{u"Astrix"_s, u"Ace Ventura"_s}), YouTubeMusicNames::SplitCollaboration(u"Astrix feat. Ace Ventura"_s));
  EXPECT_TRUE(YouTubeMusicNames::SplitCollaboration(u"Stromae"_s).isEmpty());

}

TEST(YouTubeMusicNamesTest, PrefersTheLongestSubscribedName) {

  // The nested case that broke the first implementation: splitting gives "Chase"/"Status",
  // which are not subscribed, and the row was dropped instead of folded.
  EXPECT_EQ(u"Chase & Status"_s, YouTubeMusicNames::BestSubscribedMatch(u"Becky Hill & Chase & Status"_s, Subscribed()));
  EXPECT_EQ(u"Rezz"_s, YouTubeMusicNames::BestSubscribedMatch(u"Rezz & PVRIS"_s, Subscribed()));
  // Two subscribed names are contained here; the longer one is the right answer.
  EXPECT_EQ(u"Berg & Vermont"_s, YouTubeMusicNames::BestSubscribedMatch(u"Berg & Vermont (BR)"_s, Subscribed()));
  EXPECT_TRUE(YouTubeMusicNames::BestSubscribedMatch(u"PVRIS"_s, Subscribed()).isEmpty());

}

TEST(YouTubeMusicNamesTest, FoldsOnlyOntoSubscribedArtists) {

  const QHash<QString, QString> subscribed = Subscribed();

  EXPECT_EQ(u"Rezz"_s, YouTubeMusicNames::FoldTarget(u"Rezz & PVRIS"_s, subscribed));
  EXPECT_EQ(u"Chase & Status"_s, YouTubeMusicNames::FoldTarget(u"Becky Hill & Chase & Status"_s, subscribed));
  // Parenthetical disambiguation from YouTube. "Berg & Vermont" is subscribed as well, so the
  // longest match wins; with only "Berg" subscribed it would fold there instead.
  EXPECT_EQ(u"Berg & Vermont"_s, YouTubeMusicNames::FoldTarget(u"Berg & Vermont (BR)"_s, subscribed));

  // A subscribed name is a real artist, never folded, even though it contains "&".
  EXPECT_TRUE(YouTubeMusicNames::FoldTarget(u"Chase & Status"_s, subscribed).isEmpty());
  // A band whose parts are not subscribed stays as it is.
  EXPECT_TRUE(YouTubeMusicNames::FoldTarget(u"Of Mice & Men"_s, subscribed).isEmpty());
  // Not a collaboration at all.
  EXPECT_TRUE(YouTubeMusicNames::FoldTarget(u"Stromae"_s, subscribed).isEmpty());
  EXPECT_TRUE(YouTubeMusicNames::FoldTarget(QString(), subscribed).isEmpty());

}

TEST(YouTubeMusicNamesTest, IgnoresVeryShortNames) {

  QHash<QString, QString> subscribed;
  subscribed.insert(u"x"_s, u"X"_s);
  // A one-character subscribed name must not match inside every collaboration.
  EXPECT_TRUE(YouTubeMusicNames::FoldTarget(u"Rezz & PVRIS"_s, subscribed).isEmpty());

}
