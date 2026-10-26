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

#include <QProcess>
#include <QTemporaryFile>
#include <QUrl>
#include <QUrlQuery>
#include <QString>
#include <QStringList>
#include <QRegularExpression>

#include "core/logging.h"
#include "core/song.h"
#include "youtubemusic/youtubemusicservice.h"
#include "youtubemusicstreamurlrequest.h"

using namespace Qt::Literals::StringLiterals;

namespace {
// Extract the video id from youtubemusic://song/<id> or a music.youtube.com URL.
QString VideoIdFromUrl(const QUrl &url) {

  if (url.scheme() == u"youtubemusic"_s) {
    // Only song rows are playable. Artist and album rows carry an
    // youtubemusic://artist/<MPLAUC…> or youtubemusic://album/<MPREb…> url so the
    // collection backend has a unique url, but that id is not a video - handing
    // it to yt-dlp produces a confusing "This video is unavailable" error.
    const QString host = url.host();
    if (host != u"song"_s && host != u"video"_s && host != u"watch"_s) return QString();
    const QStringList parts = url.path().split(u'/', Qt::SkipEmptyParts);
    return parts.isEmpty() ? QString() : parts.last();
  }

  if (url.host().contains("youtube"_L1)) {
    const QString video_id = QUrlQuery(url).queryItemValue(u"v"_s);
    if (!video_id.isEmpty()) return video_id;
    const QStringList parts = url.path().split(u'/', Qt::SkipEmptyParts);
    if (!parts.isEmpty()) return parts.last();
  }

  return QString();

}
}  // namespace

YouTubeMusicStreamURLRequest::YouTubeMusicStreamURLRequest(YouTubeMusicService *service, const QUrl &media_url, const uint id, QObject *parent)
    : QObject(parent),
      service_(service),
      media_url_(media_url),
      id_(id),
      process_(nullptr) {}

void YouTubeMusicStreamURLRequest::Process() {

  const QString video_id = VideoIdFromUrl(media_url_);
  video_id_ = video_id;
  if (video_id.isEmpty()) {
    Failure(tr("Invalid YouTube Music URL: %1").arg(media_url_.toString()));
    return;
  }

  const QString ytdlp = service_->ytdlp_path();
  if (ytdlp.isEmpty()) {
    Failure(tr("yt-dlp is not configured. Set the yt-dlp path in the YouTube Music settings."));
    return;
  }

  // YouTube refuses to hand out stream urls without a signed-in session ("sign in to
  // confirm you're not a bot"), so pass the stored cookies to yt-dlp.
  const QString cookie_path = service_ ? service_->WriteCookieFile(&cookie_file_) : QString();

  QStringList args;
  // HLS is requested first: the progressive (non-DASH/HLS) URLs YouTube hands
  // out are frequently rejected with HTTP 403 unless they come with a PO token,
  // while the HLS playlist and its segments download fine.
  args << u"--no-playlist"_s
       << u"--no-warnings"_s
       << u"--quiet"_s
       << u"--print"_s << u"duration"_s
       << u"--print"_s << u"urls"_s
       // Title/artist/album in one line (unit separator, so values may contain tabs):
       // used to fill in playlist rows that were created from a bare url.
       << u"--print"_s << u"%(title)s\u001f%(artist)s\u001f%(album)s\u001f%(release_year|)s"_s
       << u"-f"_s << u"bestaudio[protocol^=m3u8]/best[protocol^=m3u8]/bestaudio/best"_s;
  // Options must come before the "--" separator, everything after it is treated as a url.
  if (!cookie_path.isEmpty()) args << u"--cookies"_s << cookie_path;
  args << u"--"_s
       << QStringLiteral("https://music.youtube.com/watch?v=%1").arg(video_id);


  process_ = new QProcess(this);
  process_->setProgram(ytdlp);
  process_->setArguments(args);
  process_->setProcessChannelMode(QProcess::SeparateChannels);

  QObject::connect(process_, &QProcess::finished, this, &YouTubeMusicStreamURLRequest::ProcessFinished);
  QObject::connect(process_, &QProcess::errorOccurred, this, &YouTubeMusicStreamURLRequest::ProcessErrorOccurred);

  qLog(Debug) << "YouTubeMusic: resolving stream for" << video_id << "with" << ytdlp;
  process_->start();

}

void YouTubeMusicStreamURLRequest::ProcessFinished(const int exit_code, const QProcess::ExitStatus exit_status) {

  if (!process_) return;

  if (exit_status != QProcess::NormalExit || exit_code != 0) {
    const QString stderr_output = QString::fromLocal8Bit(process_->readAllStandardError()).trimmed();
    Failure(stderr_output.isEmpty() ? tr("yt-dlp failed with exit code %1.").arg(exit_code) : stderr_output);
    return;
  }

  const QStringList lines = QString::fromLocal8Bit(process_->readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts);
  QString stream_url;
  qint64 duration_seconds = -1;
  for (const QString &line : lines) {
    const QString candidate = line.trimmed();

    if (candidate.contains(u'\u001f')) {
      const QStringList parts = candidate.split(u'\u001f');
      Song metadata(Song::Source::YouTubeMusic);
      metadata.set_valid(true);
      metadata.set_url(media_url_);
      metadata.set_title(parts.value(0).trimmed());
      metadata.set_artist(parts.value(1).trimmed());
      metadata.set_artist(parts.value(1).trimmed());
      metadata.set_artistsort(parts.value(1).trimmed());
      metadata.set_album(parts.value(2).trimmed());
      metadata.set_albumartist(parts.value(1).trimmed());
      metadata.set_song_id(video_id_);
      const int year = parts.value(3).trimmed().toInt();
      if (year > 1000) metadata.set_year(year);
      if (!metadata.title().isEmpty()) Q_EMIT MetadataFound(media_url_, metadata);
      continue;
    }
    if (candidate.startsWith("http"_L1)) {
      if (stream_url.isEmpty()) stream_url = candidate;
      continue;
    }
    bool ok = false;
    const qint64 seconds = candidate.toLongLong(&ok);
    if (ok && seconds > 0 && duration_seconds == -1) duration_seconds = seconds;
  }

  if (stream_url.isEmpty()) {
    Failure(tr("yt-dlp did not return a stream URL."));
    return;
  }

  qLog(Debug) << "YouTubeMusic: resolved stream for" << media_url_.toString() << "duration" << duration_seconds;

  // Report the real length so the playlist shows it and the lyrics clients get a
  // valid duration (library rows are stored without one).
  const qint64 duration_nanosec = duration_seconds > 0 ? duration_seconds * 1000000000LL : -1;
  Q_EMIT StreamURLSuccess(id_, media_url_, QUrl(stream_url), Song::FileType::Stream, -1, -1, duration_nanosec);

}

void YouTubeMusicStreamURLRequest::ProcessErrorOccurred(const QProcess::ProcessError error) {

  Q_UNUSED(error);

  if (!process_) return;

  Failure(tr("Failed to run yt-dlp: %1").arg(process_->errorString()));

}

void YouTubeMusicStreamURLRequest::Failure(const QString &error) {

  // No deleteLater() here: the service owns this object through a shared
  // pointer whose deleter schedules the deletion when the request is dropped.
  qLog(Error) << "YouTubeMusic:" << error;
  Q_EMIT StreamURLFailure(id_, media_url_, error);

}
