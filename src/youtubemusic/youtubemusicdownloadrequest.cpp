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

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include "core/logging.h"
#include "core/song.h"
#include "youtubemusic/youtubemusicservice.h"
#include "youtubemusicdownloadrequest.h"

using namespace Qt::Literals::StringLiterals;

namespace {

}  // namespace

YouTubeMusicDownloadRequest::YouTubeMusicDownloadRequest(YouTubeMusicService *service, const Song &song, const int task_id, QObject *parent)
    : QObject(parent),
      service_(service),
      song_(song),
      task_id_(task_id),
      cancelled_(false),
      process_(nullptr) {}

namespace {

// Same rule as the stream resolver: the video id is either stored on the song or the
// last component of youtubemusic://song/<id>.
QString VideoIdForSong(const Song &song) {

  if (!song.song_id().isEmpty()) return song.song_id();

  const QUrl url = song.url();
  if (url.scheme() != u"youtubemusic"_s) return QString();
  const QStringList parts = url.path().split(u'/', Qt::SkipEmptyParts);

  return parts.isEmpty() ? QString() : parts.last();

}

}  // namespace

void YouTubeMusicDownloadRequest::Process() {

  const QString video_id = VideoIdForSong(song_);
  if (video_id.isEmpty()) {
    Finish(QString(), tr("Song has no video id."));
    return;
  }

  const QString ytdlp = service_->ytdlp_path();
  if (ytdlp.isEmpty()) {
    Finish(QString(), tr("yt-dlp is not configured."));
    return;
  }

  QString download_dir = service_->download_dir();
  if (download_dir.isEmpty()) {
    Finish(QString(), tr("No download directory is configured."));
    return;
  }
  QDir().mkpath(download_dir);

  // <folder>/<Artist>/<Album>/<Title>.<ext>. The components come from yt-dlp's own
  // metadata (the same values it embeds as tags), so the folder layout and the tags
  // can never disagree; each field has a fallback so a path component is never empty.
  // Track number: yt-dlp reports NA for these uploads, so use the number we assigned
  // when the album was expanded (if any) - otherwise omit the prefix entirely.
  const QString filename = song_.track() > 0
                               ? QStringLiteral("%1 - %(title)s.%(ext)s").arg(song_.track(), 2, 10, QLatin1Char('0'))
                               : u"%(title)s.%(ext)s"_s;
  const QString output_template = QStringLiteral("%1/%(artist,uploader|Unknown Artist)s/%(album|Unknown Album)s/%2").arg(download_dir, filename);

  const QString cookie_path = service_ ? service_->WriteCookieFile(&cookie_file_) : QString();

  QStringList args;
  args << u"--no-playlist"_s
       << u"--no-warnings"_s
       << u"--newline"_s
       << u"-x"_s
       << u"--audio-format"_s << service_->download_format()
       << u"--audio-quality"_s << u"0"_s
       << u"--embed-metadata"_s;

  if (service_->download_embed_thumbnail()) args << u"--embed-thumbnail"_s;

  // yt-dlp has no track number for these uploads, so seed the tag from the number we
  // assigned during album expansion; otherwise the file would disagree with its name.
  if (song_.track() > 0) {
    args << u"--parse-metadata"_s
         << QStringLiteral("%1:(?P<track_number>.*)").arg(song_.track());
  }

  if (!cookie_path.isEmpty()) args << u"--cookies"_s << cookie_path;

  args << u"--output"_s << output_template
       << u"--print"_s << u"after_move:filepath"_s
       << u"--"_s
       << QStringLiteral("https://music.youtube.com/watch?v=%1").arg(video_id);

  process_ = new QProcess(this);
  process_->setProgram(ytdlp);
  process_->setArguments(args);
  process_->setProcessChannelMode(QProcess::SeparateChannels);

  QObject::connect(process_, &QProcess::readyReadStandardOutput, this, &YouTubeMusicDownloadRequest::ReadyReadStandardOutput);
  QObject::connect(process_, &QProcess::finished, this, &YouTubeMusicDownloadRequest::ProcessFinished);
  QObject::connect(process_, &QProcess::errorOccurred, this, &YouTubeMusicDownloadRequest::ProcessErrorOccurred);

  qLog(Info) << "YouTubeMusic: downloading" << song_.artist() << "-" << song_.title() << "to" << download_dir;
  process_->start();

}

void YouTubeMusicDownloadRequest::Cancel() {

  cancelled_ = true;
  if (process_) process_->kill();

}

void YouTubeMusicDownloadRequest::ReadyReadStandardOutput() {

  if (!process_) return;

  // yt-dlp --newline prints lines like
  //   [download]  42.3% of ~4.30MiB at 1.20MiB/s ETA 00:03
  const QString output = QString::fromLocal8Bit(process_->readAllStandardOutput());
  static const QRegularExpression percentage(QStringLiteral("\\[(download|ExtractAudio)\\][^0-9]*([0-9]{1,3})\\.?[0-9]*%"));
  QRegularExpressionMatch match = percentage.match(output);
  if (match.hasMatch()) {
    Q_EMIT Progress(qBound(0, match.captured(2).toInt(), 100));
  }

}

void YouTubeMusicDownloadRequest::ProcessFinished(const int exit_code, const QProcess::ExitStatus exit_status) {

  if (!process_) return;

  if (cancelled_) {
    Finish(QString(), tr("Cancelled"));
    return;
  }

  if (exit_status != QProcess::NormalExit || exit_code != 0) {
    const QString stderr_output = QString::fromLocal8Bit(process_->readAllStandardError()).trimmed();
    Finish(QString(), stderr_output.isEmpty() ? tr("yt-dlp failed with exit code %1.").arg(exit_code) : stderr_output);
    return;
  }

  // --print after_move:filepath writes the final path once post-processing finished.
  QString path;
  const QStringList lines = QString::fromLocal8Bit(process_->readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts);
  for (const QString &line : lines) {
    const QString candidate = line.trimmed();
    if (candidate.startsWith(u'/') || candidate.startsWith(u'~')) path = candidate;
  }

  if (!path.isEmpty() && !QFile::exists(path)) path.clear();

  Finish(path, QString());

}

void YouTubeMusicDownloadRequest::ProcessErrorOccurred(const QProcess::ProcessError error) {

  Q_UNUSED(error);

  if (!process_) return;

  Finish(QString(), tr("Failed to run yt-dlp: %1").arg(process_->errorString()));

}

void YouTubeMusicDownloadRequest::Finish(const QString &path, const QString &error) {

  if (!error.isEmpty()) {
    qLog(Error) << "YouTubeMusic: download failed:" << error;
  }
  else {
    qLog(Info) << "YouTubeMusic: downloaded" << path;
  }

  Q_EMIT Finished(song_, path, error);

}
