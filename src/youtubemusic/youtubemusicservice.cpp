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

#include <memory>

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QTimer>
#include <QDateTime>
#include <QTemporaryFile>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QCryptographicHash>

#include "includes/shared_ptr.h"
#include "core/logging.h"
#include "core/song.h"
#include "core/settings.h"
#include "core/taskmanager.h"
#include "core/database.h"
#include "core/networkaccessmanager.h"
#include "core/urlhandlers.h"
#include "covermanager/albumcoverloader.h"
#include "collection/collectionbackend.h"
#include "collection/collectionmodel.h"
#include "streaming/streamingservice.h"
#include "constants/youtubemusicsettings.h"
#include "youtubemusic/youtubemusicrequest.h"
#include "youtubemusic/youtubemusicstreamurlrequest.h"
#include "youtubemusic/youtubemusicdownloadrequest.h"
#include "youtubemusic/youtubemusicurlhandler.h"
#include "youtubemusicservice.h"

using std::make_shared;
using namespace Qt::Literals::StringLiterals;

const Song::Source YouTubeMusicService::kSource = Song::Source::YouTubeMusic;

namespace {

// A track can be downloaded; artist/album rows are containers that must be expanded
// first. The id may live in the url only (playlist items restored from a file).
bool IsDownloadableTrack(const Song &song) {

  if (!song.song_id().isEmpty()) return true;

  const QUrl url = song.url();
  if (url.scheme() != u"youtubemusic"_s) return false;

  const QString host = url.host();

  return host == u"song"_s || host == u"video"_s || host == u"watch"_s;

}

constexpr char kArtistsSongsTable[] = "youtubemusic_artists_songs";
constexpr char kAlbumsSongsTable[] = "youtubemusic_albums_songs";
constexpr char kSongsTable[] = "youtubemusic_songs";

// The cookie is a plain "name=value; name=value" browser cookie string. Only
// SAPISID / __Secure-3PAPISID are needed to sign the InnerTube requests.
QString CookieValue(const QString &cookie, const QString &name) {

  const QStringList parts = cookie.split(u';');
  for (const QString &part : parts) {
    const QString trimmed = part.trimmed();
    if (trimmed.startsWith(name + u'=')) {
      return trimmed.mid(name.length() + 1);
    }
  }

  return QString();

}

}  // namespace

YouTubeMusicService::YouTubeMusicService(const SharedPtr<TaskManager> task_manager,
                                         const SharedPtr<Database> database,
                                         const SharedPtr<NetworkAccessManager> network,
                                         const SharedPtr<UrlHandlers> url_handlers,
                                         const SharedPtr<AlbumCoverLoader> albumcover_loader,
                                         QObject *parent)
    : StreamingService(Song::Source::YouTubeMusic, u"YouTube Music"_s, u"youtubemusic"_s, QLatin1String(YouTubeMusicSettings::kSettingsGroup), parent),
      task_manager_(task_manager),
      database_(database),
      network_(network),
      url_handler_(new YouTubeMusicUrlHandler(task_manager, this)),
      artists_collection_backend_(nullptr),
      albums_collection_backend_(nullptr),
      songs_collection_backend_(nullptr),
      artists_collection_model_(nullptr),
      albums_collection_model_(nullptr),
      songs_collection_model_(nullptr),
      timer_search_delay_(new QTimer(this)),
      enabled_(true),
      search_limit_(50),
      remove_remastered_(true),
      pending_search_id_(0),
      pending_search_type_(SearchType::Songs),
      next_pending_search_id_(1),
      search_id_(0),
      next_stream_url_request_id_(0),
      expand_into_artists_view_(false),
      expand_album_task_id_(-1),
      expand_artist_task_id_(-1),
      track_fetch_task_id_(-1),
      current_download_index_(-1),
      download_request_(nullptr),
      download_task_id_(-1) {

  url_handlers->Register(url_handler_);

  // Backends

  artists_collection_backend_ = make_shared<CollectionBackend>();
  artists_collection_backend_->moveToThread(database->thread());
  artists_collection_backend_->Init(database, task_manager, Song::Source::YouTubeMusic, QLatin1String(kArtistsSongsTable));

  albums_collection_backend_ = make_shared<CollectionBackend>();
  albums_collection_backend_->moveToThread(database->thread());
  albums_collection_backend_->Init(database, task_manager, Song::Source::YouTubeMusic, QLatin1String(kAlbumsSongsTable));

  songs_collection_backend_ = make_shared<CollectionBackend>();
  songs_collection_backend_->moveToThread(database->thread());
  songs_collection_backend_->Init(database, task_manager, Song::Source::YouTubeMusic, QLatin1String(kSongsTable));

  // Models

  artists_collection_model_ = new CollectionModel(artists_collection_backend_, albumcover_loader, this);
  albums_collection_model_ = new CollectionModel(albums_collection_backend_, albumcover_loader, this);
  songs_collection_model_ = new CollectionModel(songs_collection_backend_, albumcover_loader, this);

  // Search

  timer_search_delay_->setSingleShot(true);
  timer_search_delay_->setInterval(300);
  QObject::connect(timer_search_delay_, &QTimer::timeout, this, &YouTubeMusicService::StartSearch);

  YouTubeMusicService::ReloadSettings();

  // Self-healing: collapse artist names that only differ by capitalisation.
  NormalizeArtistNames();









}

YouTubeMusicService::~YouTubeMusicService() {

  while (!stream_url_requests_.isEmpty()) {
    YouTubeMusicStreamURLRequestPtr stream_url_request = stream_url_requests_.take(stream_url_requests_.firstKey());
    QObject::disconnect(&*stream_url_request, nullptr, this, nullptr);
  }

}

void YouTubeMusicService::ReloadSettings() {

  Settings s;
  s.beginGroup(QLatin1String(YouTubeMusicSettings::kSettingsGroup));
  enabled_ = s.value(QLatin1String(YouTubeMusicSettings::kEnabled), true).toBool();
  cookie_ = s.value(QLatin1String(YouTubeMusicSettings::kCookie)).toString().trimmed();
  ytdlp_path_ = s.value(QLatin1String(YouTubeMusicSettings::kYtdlpPath), u"yt-dlp"_s).toString().trimmed();
  search_limit_ = s.value(QLatin1String(YouTubeMusicSettings::kSearchLimit), 50).toInt();
  remove_remastered_ = s.value(QLatin1String(YouTubeMusicSettings::kRemoveRemastered), true).toBool();
  auto_refresh_cookies_ = s.value(QLatin1String(YouTubeMusicSettings::kAutoRefreshCookies), true).toBool();
  download_dir_ = s.value(QLatin1String(YouTubeMusicSettings::kDownloadDir),
                          QStringLiteral("%1/Music/YouTubeMusic").arg(QDir::homePath())).toString();
  download_format_ = s.value(QLatin1String(YouTubeMusicSettings::kDownloadFormat), u"opus"_s).toString();
  download_embed_thumbnail_ = s.value(QLatin1String(YouTubeMusicSettings::kDownloadEmbedThumbnail), true).toBool();
  cookies_browser_ = s.value(QLatin1String(YouTubeMusicSettings::kCookiesBrowser), u"chrome"_s).toString();
  cookies_script_ = s.value(QLatin1String(YouTubeMusicSettings::kCookiesScript),
                            QStringLiteral("%1/Projects/scripts/strawberry-ytm-cookies.py").arg(QDir::homePath())).toString();
  s.endGroup();

  if (search_limit_ <= 0) search_limit_ = 50;

}

bool YouTubeMusicService::authenticated() const {

  return !cookie_.isEmpty();

}

YouTubeMusicService::SessionInfo YouTubeMusicService::session_info() const {

  SessionInfo info;
  info.signed_in = !cookie_.isEmpty();

  Settings s;
  s.beginGroup(YouTubeMusicSettings::kSettingsGroup);
  info.imported_at = s.value(YouTubeMusicSettings::kCookiesImportedAt).toString();
  info.browser = s.value(YouTubeMusicSettings::kCookiesBrowser).toString();
  s.endGroup();

  if (database_) {
    QMutexLocker locker(database_->Mutex());
    QSqlDatabase db = database_->Connect();
    for (const auto &entry : {std::make_pair(u"youtubemusic_songs"_s, &info.songs),
                              std::make_pair(u"youtubemusic_artists_songs"_s, &info.artists),
                              std::make_pair(u"youtubemusic_albums_songs"_s, &info.albums)}) {
      SqlQuery q(db);
      q.prepare(QStringLiteral("SELECT count(*) FROM %1").arg(entry.first));
      if (q.Exec() && q.next()) *entry.second = q.value(0).toInt();
    }
  }

  return info;

}

void YouTubeMusicService::SignOut() {

  cookie_.clear();
  Settings s;
  s.beginGroup(YouTubeMusicSettings::kSettingsGroup);
  s.setValue(YouTubeMusicSettings::kCookie, QString());
  s.remove(YouTubeMusicSettings::kCookiesImportedAt);
  s.endGroup();
  s.sync();

  qLog(Info) << "YouTubeMusic: signed out";

}

bool YouTubeMusicService::ImportCookiesFromBrowser(const QString &browser, QString *error) {

  const QString ytdlp = ytdlp_path_.isEmpty() ? u"yt-dlp"_s : ytdlp_path_;
  if (browser.isEmpty()) {
    if (error) *error = tr("No browser selected.");
    return false;
  }

  QTemporaryFile jar;
  if (!jar.open()) {
    if (error) *error = tr("Could not create a temporary file.");
    return false;
  }
  const QString jar_path = jar.fileName();
  // yt-dlp creates the jar; it refuses an existing (empty) file as "not a Netscape cookie
  // file", so hand it the name only.
  jar.remove();

  const QString old_cookie = cookie_;

  // yt-dlp both decrypts the browser store (any supported browser, optional keyring and
  // profile) and dumps the jar; one video is enough to make it do the work.
  QProcess process;
  process.setProgram(ytdlp);
  process.setArguments({
    u"--no-warnings"_s,
    u"--skip-download"_s,
    u"--cookies-from-browser"_s, browser,
    u"--cookies"_s, jar_path,
    u"--print"_s, u"id"_s,
    u"--"_s, u"https://music.youtube.com/watch?v=dQw4w9WgXcQ"_s,
  });
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.start();
  if (!process.waitForFinished(60000)) {
    process.kill();
    if (error) *error = tr("Reading cookies from %1 timed out.").arg(browser);
    return false;
  }
  const QString output = QString::fromLocal8Bit(process.readAll()).trimmed();
  if (process.exitCode() != 0) {
    if (error) *error = output.isEmpty() ? tr("yt-dlp could not read cookies from %1.").arg(browser) : output;
    return false;
  }

  // Keep only the cookies the YouTube Music session needs, for the youtube.com domains.
  QFile file(jar_path);
  if (!file.open(QIODevice::ReadOnly)) {
    if (error) *error = tr("Could not read the exported cookies.");
    return false;
  }
  QStringList parts;
  int count = 0;
  while (!file.atEnd()) {
    const QString line = QString::fromUtf8(file.readLine()).trimmed();
    if (line.isEmpty() || line.startsWith(u'#')) continue;
    const QStringList fields = line.split(u'\t');
    if (fields.count() < 7) continue;
    const QString domain = fields.at(0);
    const QString name = fields.at(5);
    const QString value = fields.mid(6).join(u'\t');
    if (!domain.contains(u"youtube"_s)) continue;
    parts << QStringLiteral("%1=%2").arg(name, value);
    ++count;
  }

  if (count == 0) {
    if (error) *error = tr("No YouTube cookies found in %1. Sign in to music.youtube.com there first.").arg(browser);
    return false;
  }

  cookie_ = parts.join(u"; "_s);
  {
    Settings s;
    s.beginGroup(YouTubeMusicSettings::kSettingsGroup);
    s.setValue(YouTubeMusicSettings::kCookie, cookie_);
    s.setValue(YouTubeMusicSettings::kCookiesBrowser, browser);
    s.setValue(YouTubeMusicSettings::kCookiesImportedAt, QDateTime::currentDateTime().toString(Qt::ISODate));
    s.endGroup();
    s.sync();
  }

  qLog(Info) << "YouTubeMusic: imported" << count << "cookie(s) from" << browser;
  if (cookie_ == old_cookie && error) *error = tr("The cookies did not change.");
  return cookie_ != old_cookie;

}


bool YouTubeMusicService::RefreshCookies() {

  if (!auto_refresh_cookies_) return false;

  // Preferred path: read the session straight out of the browser (brave, chrome, chromium,
  // edge, firefox, opera, safari, vivaldi, whale). No keyring script, no Chrome-only code.
  if (!cookies_browser_.isEmpty()) {
    if (ImportCookiesFromBrowser(cookies_browser_)) return true;
    qLog(Warning) << "YouTubeMusic: browser import failed, falling back to the helper script";
  }

  if (cookies_script_.isEmpty() || !QFile::exists(cookies_script_)) {
    qLog(Warning) << "YouTubeMusic: cookie refresh script not found:" << cookies_script_;
    return false;
  }

  const QString old_cookie = cookie_;

  QProcess process;
  process.setProgram(u"python3"_s);
  process.setArguments({cookies_script_});
  process.setProcessChannelMode(QProcess::MergedChannels);
  process.start();
  if (!process.waitForFinished(30000) || process.exitCode() != 0) {
    qLog(Error) << "YouTubeMusic: cookie refresh failed:" << QString::fromLocal8Bit(process.readAll()).trimmed();
    return false;
  }

  // The script wrote the config behind our back; sync so we read the new value.
  {
    Settings s;
    s.sync();
  }
  ReloadSettings();

  if (cookie_.isEmpty() || cookie_ == old_cookie) return false;

  qLog(Info) << "YouTubeMusic: cookies refreshed from the browser";
  return true;

}

QByteArray YouTubeMusicService::authorization_header() const {

  QString sapisid = CookieValue(cookie_, u"SAPISID"_s);
  if (sapisid.isEmpty()) sapisid = CookieValue(cookie_, u"__Secure-3PAPISID"_s);
  if (sapisid.isEmpty()) return QByteArray();

  const QString timestamp = QString::number(QDateTime::currentSecsSinceEpoch());
  const QByteArray hash = QCryptographicHash::hash((timestamp + u' ' + sapisid + u' ' + u"https://music.youtube.com"_s).toUtf8(), QCryptographicHash::Sha1).toHex();

  return "SAPISIDHASH " + timestamp.toUtf8() + '_' + hash;

}

void YouTubeMusicService::Exit() {

  wait_for_exit_ << &*artists_collection_backend_ << &*albums_collection_backend_ << &*songs_collection_backend_;

  QObject::connect(&*artists_collection_backend_, &CollectionBackend::ExitFinished, this, &YouTubeMusicService::ExitReceived);
  QObject::connect(&*albums_collection_backend_, &CollectionBackend::ExitFinished, this, &YouTubeMusicService::ExitReceived);
  QObject::connect(&*songs_collection_backend_, &CollectionBackend::ExitFinished, this, &YouTubeMusicService::ExitReceived);

  artists_collection_backend_->ExitAsync();
  albums_collection_backend_->ExitAsync();
  songs_collection_backend_->ExitAsync();

}

void YouTubeMusicService::ExitReceived() {

  QObject *obj = sender();
  QObject::disconnect(obj, nullptr, this, nullptr);
  wait_for_exit_.removeAll(obj);
  if (wait_for_exit_.isEmpty()) Q_EMIT ExitFinished();

}

uint YouTubeMusicService::GetStreamURL(const QUrl &url, QString &error) {

  if (ytdlp_path_.isEmpty()) {
    error = tr("yt-dlp is not configured.");
    return 0;
  }

  uint id = 0;
  while (id == 0) id = ++next_stream_url_request_id_;

  YouTubeMusicStreamURLRequestPtr stream_url_request = YouTubeMusicStreamURLRequestPtr(new YouTubeMusicStreamURLRequest(this, url, id), &QObject::deleteLater);
  stream_url_requests_.insert(id, stream_url_request);
  QObject::connect(&*stream_url_request, &YouTubeMusicStreamURLRequest::StreamURLFailure, this, &YouTubeMusicService::HandleStreamURLFailure);
  QObject::connect(&*stream_url_request, &YouTubeMusicStreamURLRequest::StreamURLSuccess, this, &YouTubeMusicService::HandleStreamURLSuccess);
  QObject::connect(&*stream_url_request, &YouTubeMusicStreamURLRequest::MetadataFound, this, &YouTubeMusicService::HandleStreamMetadata);
  stream_url_request->Process();

  return id;

}

void YouTubeMusicService::HandleStreamMetadata(const QUrl &media_url, const Song &song) {

  Q_UNUSED(media_url);

  Q_EMIT StreamMetadataResolved(song);

}

void YouTubeMusicService::HandleStreamURLFailure(const uint id, const QUrl &media_url, const QString &error) {

  if (!stream_url_requests_.contains(id)) return;
  stream_url_requests_.remove(id);

  Q_EMIT StreamURLFailure(id, media_url, error);

}

void YouTubeMusicService::HandleStreamURLSuccess(const uint id, const QUrl &media_url, const QUrl &stream_url, const Song::FileType filetype, const int samplerate, const int bit_depth, const qint64 duration) {

  if (!stream_url_requests_.contains(id)) return;
  stream_url_requests_.remove(id);

  Q_EMIT StreamURLSuccess(id, media_url, stream_url, filetype, samplerate, bit_depth, duration);

}

int YouTubeMusicService::Search(const QString &text, const SearchType type) {

  pending_search_id_ = next_pending_search_id_;
  pending_search_text_ = text;
  pending_search_type_ = type;

  next_pending_search_id_++;

  if (text.isEmpty()) {
    timer_search_delay_->stop();
    return pending_search_id_;
  }

  timer_search_delay_->start();

  return pending_search_id_;

}

void YouTubeMusicService::CancelSearch() {}

void YouTubeMusicService::StartSearch() {

  search_id_ = pending_search_id_;
  search_text_ = pending_search_text_;

  SendSearch();

}

void YouTubeMusicService::SendSearch() {

  YouTubeMusicRequest::Type type = YouTubeMusicRequest::Type::None;

  switch (pending_search_type_) {
    case SearchType::Artists:
      type = YouTubeMusicRequest::Type::SearchArtists;
      break;
    case SearchType::Albums:
      type = YouTubeMusicRequest::Type::SearchAlbums;
      break;
    case SearchType::Songs:
      type = YouTubeMusicRequest::Type::SearchSongs;
      break;
    default:
      return;
  }

  search_request_.reset(new YouTubeMusicRequest(this, network_, this));
  QObject::connect(&*search_request_, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::SearchResultsReceived);
  QObject::connect(&*search_request_, &YouTubeMusicRequest::UpdateStatus, this, &YouTubeMusicService::SearchUpdateStatusReceived);

  search_request_->Process(type, search_id_, search_text_);

}

void YouTubeMusicService::SearchResultsReceived(const int id, const SongMap &songs, const QString &error) {

  Q_UNUSED(id);

  Q_EMIT SearchResults(id, songs, error);
  search_request_.reset();

}

void YouTubeMusicService::SearchUpdateStatusReceived(const QString &text) {

  Q_EMIT SearchUpdateStatus(search_id_, text);

}

void YouTubeMusicService::GetArtists() {

  if (!authenticated()) {
    Q_EMIT ArtistsResults(SongMap(), tr("Not logged in to YouTube Music. Paste your cookies in the YouTube Music settings to see your library."));
    return;
  }

  artists_request_.reset(new YouTubeMusicRequest(this, network_, this));
  QObject::connect(&*artists_request_, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::ArtistsResultsReceived);
  QObject::connect(&*artists_request_, &YouTubeMusicRequest::UpdateStatus, this, &YouTubeMusicService::ArtistsUpdateStatusReceived);

  artists_request_->Process(YouTubeMusicRequest::Type::LibraryArtists);

}

void YouTubeMusicService::GetAlbums() {

  if (!authenticated()) {
    Q_EMIT AlbumsResults(SongMap(), tr("Not logged in to YouTube Music. Paste your cookies in the YouTube Music settings to see your library."));
    return;
  }

  albums_request_.reset(new YouTubeMusicRequest(this, network_, this));
  QObject::connect(&*albums_request_, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::AlbumsResultsReceived);
  QObject::connect(&*albums_request_, &YouTubeMusicRequest::UpdateStatus, this, &YouTubeMusicService::AlbumsUpdateStatusReceived);

  albums_request_->Process(YouTubeMusicRequest::Type::LibraryAlbums);

}

void YouTubeMusicService::GetSongs() {

  if (!authenticated()) {
    Q_EMIT SongsResults(SongMap(), tr("Not logged in to YouTube Music. Paste your cookies in the YouTube Music settings to see your library."));
    return;
  }

  songs_request_.reset(new YouTubeMusicRequest(this, network_, this));
  QObject::connect(&*songs_request_, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::SongsResultsReceived);
  QObject::connect(&*songs_request_, &YouTubeMusicRequest::UpdateStatus, this, &YouTubeMusicService::SongsUpdateStatusReceived);

  songs_request_->Process(YouTubeMusicRequest::Type::LibrarySongs);

}

void YouTubeMusicService::GetAlbumTracks(const Song &album) {

  if (album.album_id().isEmpty()) return;

  track_fetch_task_id_ = task_manager_->StartTask(tr("Loading tracks of %1").arg(album.album().isEmpty() ? album.album_id() : album.album()));

  album_tracks_request_.reset(new YouTubeMusicRequest(this, network_, this));
  QObject::connect(&*album_tracks_request_, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::AlbumTracksResultsReceived);
  QObject::connect(&*album_tracks_request_, &YouTubeMusicRequest::UpdateStatus, this, &YouTubeMusicService::SongsUpdateStatusReceived);

  album_tracks_request_->ProcessAlbum(album);

}

void YouTubeMusicService::GetArtistTracks(const Song &artist) {

  if (artist.artist_id().isEmpty()) return;

  track_fetch_task_id_ = task_manager_->StartTask(tr("Loading tracks of %1").arg(artist.artist()));

  artist_tracks_request_.reset(new YouTubeMusicRequest(this, network_, this));
  QObject::connect(&*artist_tracks_request_, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::AlbumTracksResultsReceived);
  QObject::connect(&*artist_tracks_request_, &YouTubeMusicRequest::UpdateStatus, this, &YouTubeMusicService::SongsUpdateStatusReceived);

  artist_tracks_request_->ProcessArtist(artist);

}

void YouTubeMusicService::DownloadSongs(const SongList &songs) {

  for (const Song &song : songs) {
    if (IsDownloadableTrack(song)) {
      DownloadEntry entry;
      entry.song = song;
      download_entries_ << entry;
    }
    else if (!song.album_id().isEmpty() || !song.artist_id().isEmpty()) {
      // "Download" on an album/artist row means: fetch its tracks first.
      download_containers_ << song;
    }
  }

  Q_EMIT DownloadQueueChanged();

  if (download_container_request_ == nullptr && !download_containers_.isEmpty()) StartNextDownloadContainer();
  if (download_request_ == nullptr && !download_entries_.isEmpty()) StartNextDownload();

}

QString YouTubeMusicService::WriteCookieFile(QTemporaryFile *file) const {

  if (cookie_.isEmpty() || !file) return QString();

  file->setAutoRemove(true);
  if (!file->open()) {
    qLog(Error) << "YouTubeMusic: could not create a cookies file for yt-dlp";
    return QString();
  }

  // Netscape cookies.txt. The HttpOnly prefix is what yt-dlp/curl expect for the session
  // cookies YouTube sets (SID/HSID/SSID/APISID/SAPISID/__Secure-*).
  QByteArray data("# Netscape HTTP Cookie File\n");
  const qint64 expiry = QDateTime::currentSecsSinceEpoch() + (86400LL * 365);
  for (const QString &part : cookie_.split(u';', Qt::SkipEmptyParts)) {
    const QString item = part.trimmed();
    const int equals = item.indexOf(u'=');
    if (equals <= 0) continue;
    const QString name = item.left(equals).trimmed();
    const QString value = item.mid(equals + 1);
    if (name.isEmpty()) continue;
    data += QStringLiteral("#HttpOnly_.youtube.com\tTRUE\t/\tTRUE\t%1\t%2\t%3\n").arg(expiry).arg(name, value).toUtf8();
  }
  file->write(data);
  file->flush();
  qLog(Debug) << "YouTubeMusic: wrote" << file->fileName() << "for yt-dlp with" << cookie_.count(u'=') << "cookie(s)";

  return file->fileName();

}

void YouTubeMusicService::NormalizeArtistNames() {

  if (!database_) return;

  QMutexLocker locker(database_->Mutex());
  QSqlDatabase db = database_->Connect();

  // Canonical spelling: the name on the row that carries an artist id (the library row).
  QHash<QString, QString> canonical;
  {
    SqlQuery q(db);
    q.prepare(QStringLiteral("SELECT artist FROM youtubemusic_artists_songs WHERE artist_id IS NOT NULL AND artist_id <> '' AND artist <> ''"));
    if (!q.Exec()) {
      database_->ReportErrors(q);
      return;
    }
    while (q.next()) {
      const QString name = q.value(0).toString();
      const QString key = name.toLower();
      if (!canonical.contains(key)) canonical.insert(key, name);
    }
  }

  if (canonical.isEmpty()) return;

  int fixed = 0;
  for (const QString &table : {u"youtubemusic_artists_songs"_s, u"youtubemusic_albums_songs"_s, u"youtubemusic_songs"_s}) {
    QStringList variants;
    {
      SqlQuery q(db);
      q.prepare(QStringLiteral("SELECT DISTINCT artist FROM %1 WHERE artist <> ''").arg(table));
      if (!q.Exec()) {
        database_->ReportErrors(q);
        continue;
      }
      while (q.next()) {
        const QString name = q.value(0).toString();
        const QString target = canonical.value(name.toLower());
        if (!target.isEmpty() && target != name) variants << name;
      }
    }

    for (const QString &variant : std::as_const(variants)) {
      const QString target = canonical.value(variant.toLower());
      SqlQuery q(db);
      q.prepare(QStringLiteral("UPDATE %1 SET artist = :target, albumartist = CASE WHEN albumartist = :variant OR albumartist = '' THEN :target ELSE albumartist END WHERE artist = :variant").arg(table));
      q.BindValue(u":target"_s, target);
      q.BindValue(u":variant"_s, variant);
      if (!q.Exec()) {
        database_->ReportErrors(q);
        continue;
      }
      ++fixed;
    }
  }

  if (fixed > 0) qInfo() << "YouTubeMusic: normalized" << fixed << "artist name variant(s)";

}

void YouTubeMusicService::StartNextExpansion() {

  if (expand_pending_.isEmpty()) return;
  if (expand_album_request_ || expand_artist_request_) return;

  const Expansion expansion = expand_pending_.takeFirst();

  if (expansion.is_artist) {
    expand_artist_context_ = expansion.song;
    expand_artist_task_id_ = task_manager_->StartTask(tr("Loading albums of %1").arg(expansion.song.artist()));
    expand_artist_request_.reset(new YouTubeMusicRequest(this, network_, this));
    QObject::connect(&*expand_artist_request_, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::ExpandedArtistResultsReceived);
    expand_artist_request_->ProcessArtistAlbums(expansion.song);
  }
  else {
    expand_into_artists_view_ = expansion.artists_view;
    expand_album_context_ = expansion.song;
    expand_album_task_id_ = task_manager_->StartTask(tr("Loading tracks of %1").arg(expansion.song.album().isEmpty() ? expansion.song.album_id() : expansion.song.album()));
    expand_album_request_.reset(new YouTubeMusicRequest(this, network_, this));
    QObject::connect(&*expand_album_request_, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::ExpandedAlbumResultsReceived);
    expand_album_request_->ProcessAlbum(expansion.song);
  }

}

void YouTubeMusicService::StartNextDownloadContainer() {

  if (download_containers_.isEmpty()) return;

  const Song container = download_containers_.takeFirst();

  download_container_request_.reset(new YouTubeMusicRequest(this, network_, this));
  QObject::connect(&*download_container_request_, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::DownloadContainerTracksReceived);
  QObject::connect(&*download_container_request_, &YouTubeMusicRequest::UpdateStatus, this, &YouTubeMusicService::SongsUpdateStatusReceived);

  if (!container.album_id().isEmpty()) {
    Q_EMIT SongsUpdateStatus(tr("Fetching tracks of %1 for download...").arg(container.album().isEmpty() ? container.album_id() : container.album()));
    download_container_request_->ProcessAlbum(container);
  }
  else {
    Q_EMIT SongsUpdateStatus(tr("Fetching tracks of %1 for download...").arg(container.artist()));
    download_container_request_->ProcessArtist(container);
  }

}

void YouTubeMusicService::DownloadContainerTracksReceived(const int id, const SongMap &songs, const QString &error) {

  Q_UNUSED(id);

  if (error.isEmpty()) {
    for (const Song &song : songs) {
      if (!IsDownloadableTrack(song)) continue;
      DownloadEntry entry;
      entry.song = song;
      download_entries_ << entry;
    }
    Q_EMIT DownloadQueueChanged();
  }
  else {
    Q_EMIT SongsUpdateStatus(tr("Could not fetch tracks for download: %1").arg(error));
  }

  download_container_request_.reset();

  // Finish expanding every queued container before starting the transfers.
  if (!download_containers_.isEmpty()) {
    StartNextDownloadContainer();
    return;
  }

  if (download_request_ == nullptr && !download_entries_.isEmpty()) StartNextDownload();

}

void YouTubeMusicService::StartNextDownload() {

  int index = -1;
  for (int i = 0; i < download_entries_.count(); ++i) {
    if (download_entries_[i].state == DownloadEntry::State::Queued) {
      index = i;
      break;
    }
  }
  if (index == -1) return;

  download_entries_[index].state = DownloadEntry::State::Downloading;
  download_entries_[index].progress = 0;
  current_download_index_ = index;
  Q_EMIT DownloadQueueChanged();

  const Song song = download_entries_[index].song;

  download_task_id_ = task_manager_->StartTask(tr("Downloading %1 - %2").arg(song.artist(), song.title()));

  YouTubeMusicDownloadRequest *request = new YouTubeMusicDownloadRequest(this, song, download_task_id_, this);
  download_request_ = request;
  QObject::connect(request, &YouTubeMusicDownloadRequest::Progress, this, &YouTubeMusicService::DownloadProgress);
  QObject::connect(request, &YouTubeMusicDownloadRequest::Finished, this, &YouTubeMusicService::DownloadFinished);
  QObject::connect(request, &YouTubeMusicDownloadRequest::Finished, request, &QObject::deleteLater);
  request->Process();

}

void YouTubeMusicService::DownloadProgress(const int percent) {

  if (current_download_index_ < 0 || current_download_index_ >= download_entries_.count()) return;
  download_entries_[current_download_index_].progress = percent;
  Q_EMIT DownloadQueueChanged();

}

void YouTubeMusicService::CancelCurrentDownload() {

  if (download_request_) download_request_->Cancel();

}

void YouTubeMusicService::RetryFailedDownloads() {

  for (DownloadEntry &entry : download_entries_) {
    if (entry.state == DownloadEntry::State::Failed || entry.state == DownloadEntry::State::Cancelled) {
      entry.state = DownloadEntry::State::Queued;
      entry.progress = 0;
      entry.error.clear();
    }
  }

  Q_EMIT DownloadQueueChanged();

  if (!download_request_) StartNextDownload();

}

void YouTubeMusicService::DownloadFinished(const Song &song, const QString &path, const QString &error) {

  Q_UNUSED(song);

  if (current_download_index_ >= 0 && current_download_index_ < download_entries_.count()) {
    DownloadEntry &entry = download_entries_[current_download_index_];
    if (!error.isEmpty()) {
      entry.state = error == tr("Cancelled") ? DownloadEntry::State::Cancelled : DownloadEntry::State::Failed;
      entry.error = error;
    }
    else {
      entry.state = DownloadEntry::State::Done;
      entry.progress = 100;
      entry.path = path;
    }
    current_download_index_ = -1;
    Q_EMIT DownloadQueueChanged();
  }

  if (download_task_id_ != -1) {
    task_manager_->SetTaskFinished(download_task_id_);
    download_task_id_ = -1;
  }
  download_request_ = nullptr;

  if (!error.isEmpty()) {
    Q_EMIT SongsUpdateStatus(tr("Download failed: %1").arg(error));
  }
  else {
    Q_EMIT SongsUpdateStatus(tr("Downloaded to %1").arg(path));
    if (!path.isEmpty()) Q_EMIT SongsDownloaded(QStringList() << path);
  }

  StartNextDownload();

}

void YouTubeMusicService::ExpandAlbumRow(const Song &album, const bool artists_view) {

  if (album.album_id().isEmpty()) return;
  if (expanded_albums_.contains(album.album_id())) return;
  expanded_albums_.insert(album.album_id());

  Expansion expansion;
  expansion.song = album;
  expansion.artists_view = artists_view;
  expansion.is_artist = false;
  expand_pending_ << expansion;

  StartNextExpansion();

}

Song YouTubeMusicService::ContainerSong(const QString &id, const QString &name, const QString &parent_name, const bool artist_level) const {

  Song song(Song::Source::YouTubeMusic);
  song.set_valid(true);
  song.set_directory_id(0);

  if (artist_level) {
    song.set_url(QUrl(QStringLiteral("youtubemusic://artist/%1").arg(id)));
    song.set_artist(name);
    song.set_artist_id(id);
  }
  else {
    song.set_url(QUrl(QStringLiteral("youtubemusic://album/%1").arg(id)));
    song.set_album(name);
    song.set_album_id(id);
    song.set_artist(parent_name);
    song.set_albumartist(parent_name);
  }

  return song;

}

void YouTubeMusicService::GetAllArtistTracks(const Song &artist) {

  artist_tracks_albums_.clear();
  artist_tracks_collected_.clear();

  artist_walk_request_.reset(new YouTubeMusicRequest(this, network_, this));
  QObject::connect(&*artist_walk_request_, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::ArtistAlbumsReceived);
  QObject::connect(&*artist_walk_request_, &YouTubeMusicRequest::UpdateStatus, this, &YouTubeMusicService::SongsUpdateStatusReceived);
  Q_EMIT SongsUpdateStatus(tr("Collecting albums of %1...").arg(artist.artist()));
  artist_walk_request_->ProcessArtistAlbums(artist);

}

void YouTubeMusicService::ArtistAlbumsReceived(const int id, const SongMap &songs, const QString &error) {

  Q_UNUSED(id);

  artist_walk_request_.reset();

  if (!error.isEmpty() || songs.isEmpty()) {
    Q_EMIT AlbumTracksLoaded(SongList(), error.isEmpty() ? tr("No albums found for this artist.") : error);
    return;
  }

  for (const Song &album : songs) {
    if (!album.album_id().isEmpty()) artist_tracks_albums_ << album;
  }

  qLog(Info) << "YouTubeMusic: collecting tracks from" << artist_tracks_albums_.count() << "album(s)";
  StartNextArtistAlbumTracks();

}

void YouTubeMusicService::StartNextArtistAlbumTracks() {

  if (artist_tracks_albums_.isEmpty()) {
    Q_EMIT SongsUpdateStatus(QString());
    Q_EMIT AlbumTracksLoaded(artist_tracks_collected_, QString());
    return;
  }

  const Song album = artist_tracks_albums_.takeFirst();
  Q_EMIT SongsUpdateStatus(tr("Collecting tracks: %1 (%2 left)").arg(album.album()).arg(artist_tracks_albums_.count()));

  artist_walk_request_.reset(new YouTubeMusicRequest(this, network_, this));
  QObject::connect(&*artist_walk_request_, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::ArtistAlbumTracksReceived);
  artist_walk_request_->ProcessAlbum(album);

}

void YouTubeMusicService::ArtistAlbumTracksReceived(const int id, const SongMap &songs, const QString &error) {

  Q_UNUSED(id);

  artist_walk_request_.reset();

  if (error.isEmpty()) {
    for (const Song &song : songs) {
      if (song.song_id().isEmpty()) continue;
      const bool duplicate = std::any_of(artist_tracks_collected_.cbegin(), artist_tracks_collected_.cend(),
                                         [&song](const Song &existing) { return existing.song_id() == song.song_id(); });
      if (!duplicate) artist_tracks_collected_ << song;
    }
  }

  StartNextArtistAlbumTracks();

}

void YouTubeMusicService::ExpandArtistRow(const Song &artist) {

  if (artist.artist_id().isEmpty()) return;
  if (expanded_artists_.contains(artist.artist_id())) return;
  expanded_artists_.insert(artist.artist_id());

  Expansion expansion;
  expansion.song = artist;
  expansion.is_artist = true;
  expand_pending_ << expansion;

  StartNextExpansion();

}

void YouTubeMusicService::ExpandedAlbumResultsReceived(const int id, const SongMap &songs, const QString &error) {


  Q_UNUSED(id);

  if (expand_album_task_id_ != -1) {
    task_manager_->SetTaskFinished(expand_album_task_id_);
    expand_album_task_id_ = -1;
  }

  if (error.isEmpty() && !songs.isEmpty()) {
    // Insert the tracks into the model that backs the view the row was expanded
    // in, so they appear as children of the album container.
    if (expand_into_artists_view_) {
      // Keep the artist name of the parent row so the tracks group under it.
      SongList tracks = songs.values();
      const QString parent_artist = expand_album_context_.artist();
      if (!parent_artist.isEmpty()) {
        for (Song &track : tracks) {
          track.set_artist(parent_artist);
          track.set_albumartist(parent_artist);
        }
      }
      // A one-track album whose track carries the album's name is the same thing twice:
      // drop the album level so the track itself hangs under the artist. The album id is
      // kept, so downloads still resolve the right album.
      const QString album_name = expand_album_context_.album().trimmed();
      if (tracks.count() == 1 && !album_name.isEmpty() &&
          tracks.first().title().trimmed().compare(album_name, Qt::CaseInsensitive) == 0) {
        Song single = tracks.first();
        single.set_album(QString());
        single.set_albumsort(QString());
        tracks = SongList() << single;
        // The row that described the album would otherwise leave an empty album container
        // in the tree, so drop it as well.
        const QUrl album_url = expand_album_context_.url();
        if (album_url.isValid()) artists_collection_backend_->DeleteSongsByUrlsAsync({album_url});
      }
      artists_collection_backend_->AddOrUpdateSongsAsync(tracks);
    }
    else {
      albums_collection_backend_->AddOrUpdateSongsAsync(songs.values());
    }
  }
  else if (!error.isEmpty()) {
    Q_EMIT SongsUpdateStatus(tr("Could not load tracks: %1").arg(error));
  }

  expand_album_request_.reset();
  StartNextExpansion();

}

void YouTubeMusicService::ExpandedArtistResultsReceived(const int id, const SongMap &songs, const QString &error) {

  Q_UNUSED(id);


  if (expand_artist_task_id_ != -1) {
    task_manager_->SetTaskFinished(expand_artist_task_id_);
    expand_artist_task_id_ = -1;
  }

  if (error.isEmpty() && !songs.isEmpty()) {
    // Name the albums after the artist row that was expanded: YouTube reports the same
    // artist with different capitalisation on different pages (aKing / aKING), which
    // would otherwise split the artist into two containers.
    SongList albums = songs.values();
    if (!expand_artist_context_.artist().isEmpty()) {
      for (Song &album : albums) {
        album.set_artist(expand_artist_context_.artist());
        album.set_albumartist(expand_artist_context_.artist());
      }
    }
    artists_collection_backend_->AddOrUpdateSongsAsync(albums);
  }
  else if (!error.isEmpty()) {
    Q_EMIT SongsUpdateStatus(tr("Could not load albums: %1").arg(error));
  }

  expand_artist_request_.reset();
  StartNextExpansion();

}

void YouTubeMusicService::AlbumTracksResultsReceived(const int id, const SongMap &songs, const QString &error) {

  Q_UNUSED(id);

  if (track_fetch_task_id_ != -1) {
    task_manager_->SetTaskFinished(track_fetch_task_id_);
    track_fetch_task_id_ = -1;
  }

  Q_EMIT AlbumTracksLoaded(songs.values(), error);
  album_tracks_request_.reset();
  artist_tracks_request_.reset();

}

void YouTubeMusicService::ResetArtistsRequest() {

  artists_request_.reset();

}

void YouTubeMusicService::ResetAlbumsRequest() {

  albums_request_.reset();

}

void YouTubeMusicService::ResetSongsRequest() {

  songs_request_.reset();

}

void YouTubeMusicService::ArtistsResultsReceived(const int id, const SongMap &songs, const QString &error) {

  Q_UNUSED(id);

  Q_EMIT ArtistsResults(songs, error);
  ResetArtistsRequest();

}

void YouTubeMusicService::AlbumsResultsReceived(const int id, const SongMap &songs, const QString &error) {

  Q_UNUSED(id);

  Q_EMIT AlbumsResults(songs, error);
  ResetAlbumsRequest();

}

void YouTubeMusicService::SongsResultsReceived(const int id, const SongMap &songs, const QString &error) {

  Q_UNUSED(id);

  Q_EMIT SongsResults(songs, error);
  ResetSongsRequest();

}

void YouTubeMusicService::ArtistsUpdateStatusReceived(const QString &text) {

  Q_EMIT ArtistsUpdateStatus(text);

}

void YouTubeMusicService::AlbumsUpdateStatusReceived(const QString &text) {

  Q_EMIT AlbumsUpdateStatus(text);

}

void YouTubeMusicService::SongsUpdateStatusReceived(const QString &text) {

  Q_EMIT SongsUpdateStatus(text);

}
