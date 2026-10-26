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

#include <QString>
#include <QUrl>

#include "includes/shared_ptr.h"
#include "core/taskmanager.h"
#include "core/song.h"
#include "youtubemusic/youtubemusicservice.h"
#include "youtubemusicurlhandler.h"

using namespace Qt::Literals::StringLiterals;

YouTubeMusicUrlHandler::YouTubeMusicUrlHandler(const SharedPtr<TaskManager> task_manager, YouTubeMusicService *service)
    : UrlHandler(service),
      task_manager_(task_manager),
      service_(service) {

  QObject::connect(service, &YouTubeMusicService::StreamURLFailure, this, &YouTubeMusicUrlHandler::GetStreamURLFailure);
  QObject::connect(service, &YouTubeMusicService::StreamURLSuccess, this, &YouTubeMusicUrlHandler::GetStreamURLSuccess);

}

QString YouTubeMusicUrlHandler::scheme() const {

  return service_->url_scheme();

}

UrlHandler::LoadResult YouTubeMusicUrlHandler::StartLoading(const QUrl &url) {

  // Artist and album rows carry a youtubemusic:// url only so the collection table
  // has something to store; they are containers, not media. They can end up in a
  // playlist (restored from an older session, or added via the context menu), so
  // skip over them quietly instead of failing the track.
  const QString host = url.host();
  if (host != u"song"_s && host != u"video"_s && host != u"watch"_s) {
    return LoadResult(url, LoadResult::Type::NoMoreTracks);
  }

  Request request;
  request.task_id = task_manager_->StartTask(tr("Loading YouTube Music stream..."));
  QString error;
  request.id = service_->GetStreamURL(url, error);
  if (request.id == 0) {
    CancelTask(request.task_id);
    return LoadResult(url, LoadResult::Type::Error, error);
  }

  requests_.insert(request.id, request);

  LoadResult ret(url);
  ret.type_ = LoadResult::Type::WillLoadAsynchronously;

  return ret;

}

void YouTubeMusicUrlHandler::GetStreamURLFailure(const uint id, const QUrl &media_url, const QString &error) {

  if (!requests_.contains(id)) return;
  Request req = requests_.take(id);
  CancelTask(req.task_id);

  Q_EMIT AsyncLoadComplete(LoadResult(media_url, LoadResult::Type::Error, error));

}

void YouTubeMusicUrlHandler::GetStreamURLSuccess(const uint id, const QUrl &media_url, const QUrl &stream_url, const Song::FileType filetype, const int samplerate, const int bit_depth, const qint64 duration) {

  if (!requests_.contains(id)) return;
  Request req = requests_.take(id);
  CancelTask(req.task_id);

  Q_EMIT AsyncLoadComplete(LoadResult(media_url, LoadResult::Type::TrackAvailable, stream_url, filetype, samplerate, bit_depth, duration));

}

void YouTubeMusicUrlHandler::CancelTask(const int task_id) {

  task_manager_->SetTaskFinished(task_id);

}
