/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICWALK_H
#define YOUTUBEMUSICWALK_H

#include "config.h"

#include <QObject>
#include <QHash>
#include <QList>
#include <QString>

#include <functional>

#include "core/song.h"

class YouTubeMusicRequest;
class YouTubeMusicService;

// Runs one request per item with a bounded number in flight and hands every result back with
// the index it belongs to, so callers never have to track slots themselves. Three walks used to
// duplicate that bookkeeping (artist tracks, downloads, the indexer) and one of them retired the
// wrong request because of it.
class YouTubeMusicWalk : public QObject {
  Q_OBJECT

 public:
  // Creates and starts the request for one item. The walk takes ownership of what it returns.
  using Start = std::function<YouTubeMusicRequest *(const int index, const Song &item)>;
  // Called for every finished item, in completion order; index identifies the item.
  using Result = std::function<void(const int index, const SongMap &songs, const QString &error)>;
  using Finished = std::function<void()>;

  explicit YouTubeMusicWalk(YouTubeMusicService *service, QObject *parent = nullptr);

  // Starts walking items. concurrency items are in flight at a time; finished() runs once all
  // of them have reported, and never for an empty list.
  void Run(const SongList &items, const int concurrency, Start start, Result result, Finished finished);
  // Drops every request in flight; finished() will not run afterwards.
  void Cancel();
  bool is_running() const { return running_; }
  int count() const { return items_.count(); }
  int finished_count() const { return finished_count_; }

 private:
  void StartNext();
  void SlotFinished(const int index, const SongMap &songs, const QString &error);

  YouTubeMusicService *service_;
  SongList items_;
  int concurrency_ = 3;
  bool running_ = false;
  bool cancelled_ = false;
  int next_ = 0;
  int finished_count_ = 0;
  QHash<int, YouTubeMusicRequest *> in_flight_;   // parented, deleteLater'd
  Start start_;
  Result result_;
  Finished finished_;

};

#endif  // YOUTUBEMUSICWALK_H
