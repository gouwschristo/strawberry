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
#include "youtubemusicwalk.h"

#include <utility>

#include <QObject>
#include <QString>

#include "core/logging.h"
#include "core/song.h"
#include "youtubemusicrequest.h"

YouTubeMusicWalk::YouTubeMusicWalk(YouTubeMusicService *service, QObject *parent)
    : QObject(parent), service_(service) {}

void YouTubeMusicWalk::Run(const SongList &items, const int concurrency, Start start, Result result, Finished finished) {

  Cancel();

  items_ = items;
  concurrency_ = std::max(1, concurrency);
  start_ = std::move(start);
  result_ = std::move(result);
  finished_ = std::move(finished);
  next_ = 0;
  finished_count_ = 0;
  cancelled_ = false;
  running_ = true;

  StartNext();

}

void YouTubeMusicWalk::StartNext() {

  if (!running_) return;

  while (in_flight_.size() < concurrency_ && next_ < items_.count()) {
    const int index = next_++;
    YouTubeMusicRequest *request = start_ ? start_(index, items_.at(index)) : nullptr;
    if (!request) {
      // Nothing to fetch for this item; count it as finished so the walk can complete.
      ++finished_count_;
      continue;
    }
    in_flight_.insert(index, request);
    QObject::connect(request, &YouTubeMusicRequest::Results, this,
                     [this, index](const int, const SongMap &songs, const QString &error) {
                       SlotFinished(index, songs, error);
                     });
  }

  if (in_flight_.isEmpty() && next_ >= items_.count()) {
    running_ = false;
    if (finished_) {
      Finished finished = finished_;
      finished_ = Finished();
      finished();
    }
  }

}

void YouTubeMusicWalk::SlotFinished(const int index, const SongMap &songs, const QString &error) {

  if (YouTubeMusicRequest *request = in_flight_.take(index)) request->deleteLater();

  ++finished_count_;
  if (result_) result_(index, songs, error);

  if (cancelled_) return;

  StartNext();

}

void YouTubeMusicWalk::Cancel() {

  cancelled_ = true;
  running_ = false;

  for (YouTubeMusicRequest *request : std::as_const(in_flight_)) {
    if (request) request->deleteLater();
  }
  in_flight_.clear();

  start_ = Start();
  result_ = Result();
  finished_ = Finished();

}
