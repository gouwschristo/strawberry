/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICDOWNLOADREQUEST_H
#define YOUTUBEMUSICDOWNLOADREQUEST_H

#include "config.h"

#include <QObject>
#include <QProcess>
#include <QTemporaryFile>
#include <QString>

#include "core/song.h"

class YouTubeMusicService;

// Saves one song to disk with yt-dlp (audio extraction + tagging happens in yt-dlp's
// ffmpeg post-processing).
class YouTubeMusicDownloadRequest : public QObject {
  Q_OBJECT

 public:
  explicit YouTubeMusicDownloadRequest(YouTubeMusicService *service, const Song &song, const int task_id, QObject *parent = nullptr);

  void Process();
  void Cancel();

 Q_SIGNALS:
  void Progress(const int percent);
  void Finished(const Song &song, const QString &path, const QString &error);

 private Q_SLOTS:
  void ReadyReadStandardOutput();
  void ProcessFinished(const int exit_code, const QProcess::ExitStatus exit_status);
  void ProcessErrorOccurred(const QProcess::ProcessError error);

 private:
  void Finish(const QString &path, const QString &error);

  YouTubeMusicService *service_;
  Song song_;
  int task_id_;
  bool cancelled_;
  QProcess *process_;
  QTemporaryFile cookie_file_;
};

#endif  // YOUTUBEMUSICDOWNLOADREQUEST_H
