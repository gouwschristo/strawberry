/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICSTREAMURLREQUEST_H
#define YOUTUBEMUSICSTREAMURLREQUEST_H

#include "config.h"

#include <QObject>
#include <QProcess>
#include <QTemporaryFile>
#include <QUrl>
#include <QString>

#include "core/song.h"

class YouTubeMusicService;

// Resolves a playable audio URL for a YouTube Music video by asking yt-dlp.
// yt-dlp is used deliberately: resolving the stream natively means implementing
// YouTube's signature cipher, which changes without notice.
class YouTubeMusicStreamURLRequest : public QObject {
  Q_OBJECT

 public:
  explicit YouTubeMusicStreamURLRequest(YouTubeMusicService *service, const QUrl &media_url, const uint id, QObject *parent = nullptr);

  void Process();

 Q_SIGNALS:
  void MetadataFound(const QUrl &media_url, const Song &song);
  void StreamURLFailure(const uint id, const QUrl &media_url, const QString &error);
  void StreamURLSuccess(const uint id, const QUrl &media_url, const QUrl &stream_url, const Song::FileType filetype, const int samplerate, const int bit_depth, const qint64 duration);

 private Q_SLOTS:
  void ProcessFinished(const int exit_code, const QProcess::ExitStatus exit_status);
  void ProcessErrorOccurred(const QProcess::ProcessError error);

 private:
  void Failure(const QString &error);

  YouTubeMusicService *service_;
  QUrl media_url_;
  QString video_id_;
  uint id_;
  QProcess *process_;
  QTemporaryFile cookie_file_;
};

#endif  // YOUTUBEMUSICSTREAMURLREQUEST_H
