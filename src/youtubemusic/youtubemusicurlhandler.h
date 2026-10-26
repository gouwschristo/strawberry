/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICURLHANDLER_H
#define YOUTUBEMUSICURLHANDLER_H

#include "config.h"

#include <QObject>
#include <QString>
#include <QUrl>
#include <QMap>

#include "includes/shared_ptr.h"
#include "core/urlhandler.h"
#include "core/song.h"

class TaskManager;
class YouTubeMusicService;

class YouTubeMusicUrlHandler : public UrlHandler {
  Q_OBJECT

 public:
  explicit YouTubeMusicUrlHandler(const SharedPtr<TaskManager> task_manager, YouTubeMusicService *service);

  QString scheme() const override;
  LoadResult StartLoading(const QUrl &url) override;

 private Q_SLOTS:
  void GetStreamURLFailure(const uint id, const QUrl &media_url, const QString &error);
  void GetStreamURLSuccess(const uint id, const QUrl &media_url, const QUrl &stream_url, const Song::FileType filetype, const int samplerate, const int bit_depth, const qint64 duration);

 private:
  void CancelTask(const int task_id);

  struct Request {
    Request() : id(0), task_id(-1) {}
    uint id;
    int task_id;
  };

  const SharedPtr<TaskManager> task_manager_;
  YouTubeMusicService *service_;
  QMap<uint, Request> requests_;
};

#endif  // YOUTUBEMUSICURLHANDLER_H
