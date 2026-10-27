/*
 * Strawberry Music Player
 * YouTube Music walk tests
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "config.h"

#include "gtest_include.h"

#include <algorithm>

#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QString>
#include <QVector>

#include "core/song.h"
#include "includes/shared_ptr.h"
#include "youtubemusic/youtubemusicrequest.h"
#include "youtubemusic/youtubemusicwalk.h"

using namespace Qt::Literals::StringLiterals;

namespace {

// YouTubeMusicRequest's constructor only stores its pointers, so a walk can be driven without a
// session or a network: the test decides when (and in which order) each request reports.
class FakeRequest : public YouTubeMusicRequest {
 public:
  // The counter belongs to the harness that created the request, so tests cannot affect each
  // other through a shared static.
  explicit FakeRequest(int id, int *alive, QVector<int> *deleted)
      : YouTubeMusicRequest(nullptr, SharedPtr<NetworkAccessManager>(), nullptr), id_(id), alive_(alive), deleted_(deleted) {
    ++*alive_;
  }
  ~FakeRequest() override {
    --*alive_;
    deleted_->append(id_);
  }

  void Finish(const QString &error = QString()) { Q_EMIT Results(0, SongMap(), error); }

 private:
  int id_;
  int *alive_;
  QVector<int> *deleted_;

};

SongMap Songs(int count) {
  SongMap songs;
  for (int i = 0; i < count; ++i) {
    Song song;
    song.set_title(u"Track %1"_s.arg(i));
    song.set_song_id(QString::number(i));
    songs.insert(song.song_id(), song);
  }
  return songs;
}

// Starts a walk over `count` items and keeps the requests it created so the test can finish them
// in any order. YouTubeMusicWalk does not parent them; it only calls deleteLater().
struct Harness {
  QVector<FakeRequest *> started;
  // Requests this harness created that have not been destroyed yet, and the order in which the
  // walk released them (ids are creation order).
  int alive = 0;
  QVector<int> deleted;
  QVector<int> result_indexes;
  QVector<QString> errors;
  int finished = 0;
  YouTubeMusicWalk *walk = nullptr;

  explicit Harness(YouTubeMusicWalk *w, const int count) : walk(w) {
    SongList items;
    for (int i = 0; i < count; ++i) {
      Song item;
      item.set_album_id(u"album%1"_s.arg(i));
      items << item;
    }
    walk->Run(
        items, 2,
        [this](const int, const Song &) -> YouTubeMusicRequest * {
          auto *request = new FakeRequest(static_cast<int>(started.count()), &alive, &deleted);
          started << request;
          return request;
        },
        [this](const int index, const SongMap &, const QString &error) {
          result_indexes << index;
          errors << error;
        },
        [this]() { ++finished; });
  }
};

}  // namespace

TEST(YouTubeMusicWalkTest, KeepsOnlyTheConfiguredNumberInFlight) {

  YouTubeMusicWalk walk(nullptr);
  Harness harness(&walk, 5);

  EXPECT_EQ(2, harness.started.count());
  EXPECT_EQ(0, harness.finished);
  EXPECT_TRUE(walk.is_running());

}

TEST(YouTubeMusicWalkTest, AttributesEachResultToItsOwnItemInAnyOrder) {

  YouTubeMusicWalk walk(nullptr);
  Harness harness(&walk, 4);

  // Two in flight: finish the second first. A walk that retired the lowest slot instead of the
  // one that answered is what once hung the indexer.
  ASSERT_EQ(2, harness.started.count());
  harness.started.at(1)->Finish();
  EXPECT_EQ(3, harness.started.count());   // the next item started
  harness.started.at(0)->Finish();
  EXPECT_EQ(4, harness.started.count());
  harness.started.at(2)->Finish();
  harness.started.at(3)->Finish();

  EXPECT_EQ((QVector<int>{1, 0, 2, 3}), harness.result_indexes);
  EXPECT_EQ(1, harness.finished);
  EXPECT_FALSE(walk.is_running());
  EXPECT_EQ(4, walk.finished_count());

  // Every request must be released, and each must be the one that reported: a walk that retired
  // the lowest slot instead of the answering one releases them in creation order, which is how
  // the indexer once stalled with a request nobody would retire.
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  EXPECT_EQ(0, harness.alive);
  EXPECT_EQ((QVector<int>{1, 0, 2, 3}), harness.deleted);

}

TEST(YouTubeMusicWalkTest, FinishesImmediatelyForAnEmptyList) {

  YouTubeMusicWalk walk(nullptr);
  Harness harness(&walk, 0);

  EXPECT_EQ(0, harness.started.count());
  EXPECT_EQ(1, harness.finished);
  EXPECT_FALSE(walk.is_running());

}

TEST(YouTubeMusicWalkTest, PassesTheErrorMessageThrough) {

  YouTubeMusicWalk walk(nullptr);
  Harness harness(&walk, 1);

  ASSERT_EQ(1, harness.started.count());
  harness.started.first()->Finish(u"No results."_s);

  ASSERT_EQ(1, harness.errors.count());
  EXPECT_EQ(u"No results."_s, harness.errors.first());
  EXPECT_EQ(1, harness.finished);

}

TEST(YouTubeMusicWalkTest, CancelStopsEverythingWithoutFinishing) {

  YouTubeMusicWalk walk(nullptr);
  Harness harness(&walk, 5);
  ASSERT_EQ(2, harness.started.count());

  walk.Cancel();
  EXPECT_FALSE(walk.is_running());

  // Requests that were in flight may still report; the walk must not start more, must not pass
  // their results on and must not announce completion.
  for (FakeRequest *request : std::as_const(harness.started)) request->Finish();

  EXPECT_EQ(2, harness.started.count());
  EXPECT_EQ(0, harness.result_indexes.count());
  EXPECT_EQ(0, harness.finished);

}
