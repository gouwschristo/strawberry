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
  MergeCollaborationArtists();

  // YouTube rotates the session cookies hourly, and importing them takes a few seconds
  // (yt-dlp reads the browser store). Do that in the background shortly after start instead
  // of on the first request that happens to need it.
  QMetaObject::invokeMethod(this, &YouTubeMusicService::RefreshCookiesInBackground, Qt::QueuedConnection);









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
  prefetch_ = s.value(QLatin1String(YouTubeMusicSettings::kPrefetchIndex), true).toBool();
  if (!prefetch_timer_) {
    prefetch_timer_ = new QTimer(this);
    prefetch_timer_->setInterval(3 * 60 * 1000);
    QObject::connect(prefetch_timer_, &QTimer::timeout, this, &YouTubeMusicService::PrefetchNextArtist);
  }
  if (prefetch_) prefetch_timer_->start(); else prefetch_timer_->stop();

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

void YouTubeMusicService::RefreshCookiesInBackground() {

  if (!auto_refresh_cookies_) return;

  Settings s;
  s.beginGroup(YouTubeMusicSettings::kSettingsGroup);
  const QString imported_at = s.value(YouTubeMusicSettings::kCookiesImportedAt).toString();
  s.endGroup();

  const QDateTime imported = QDateTime::fromString(imported_at, Qt::ISODate);
  if (imported.isValid() && imported.secsTo(QDateTime::currentDateTime()) < 30 * 60) {
    qLog(Debug) << "YouTubeMusic: cookies are fresh, skipping the background import";
    return;
  }

  qLog(Info) << "YouTubeMusic: refreshing cookies in the background";
  if (RefreshCookies()) {
    qLog(Info) << "YouTubeMusic: background cookie refresh done";
  }

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
  if (page_budget_ > 0) (*artists_request_).set_page_budget(page_budget_);
  QObject::connect(&*artists_request_, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::ArtistsResultsReceived);
  QObject::connect(&*artists_request_, &YouTubeMusicRequest::PageReady, this, [this](const int, const SongMap &songs) {
    if (!songs.isEmpty()) Q_EMIT ArtistsResults(songs, QString());
  });
  QObject::connect(&*artists_request_, &YouTubeMusicRequest::UpdateStatus, this, &YouTubeMusicService::ArtistsUpdateStatusReceived);

  artists_request_->Process(YouTubeMusicRequest::Type::LibraryArtists);

}

void YouTubeMusicService::GetAlbums() {

  if (!authenticated()) {
    Q_EMIT AlbumsResults(SongMap(), tr("Not logged in to YouTube Music. Paste your cookies in the YouTube Music settings to see your library."));
    return;
  }

  albums_request_.reset(new YouTubeMusicRequest(this, network_, this));
  if (page_budget_ > 0) (*albums_request_).set_page_budget(page_budget_);
  QObject::connect(&*albums_request_, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::AlbumsResultsReceived);
  QObject::connect(&*albums_request_, &YouTubeMusicRequest::PageReady, this, [this](const int, const SongMap &songs) {
    if (!songs.isEmpty()) Q_EMIT AlbumsResults(songs, QString());
  });
  QObject::connect(&*albums_request_, &YouTubeMusicRequest::UpdateStatus, this, &YouTubeMusicService::AlbumsUpdateStatusReceived);

  albums_request_->Process(YouTubeMusicRequest::Type::LibraryAlbums);

}

void YouTubeMusicService::GetSongs() {

  if (!authenticated()) {
    Q_EMIT SongsResults(SongMap(), tr("Not logged in to YouTube Music. Paste your cookies in the YouTube Music settings to see your library."));
    return;
  }

  songs_request_.reset(new YouTubeMusicRequest(this, network_, this));
  if (page_budget_ > 0) (*songs_request_).set_page_budget(page_budget_);
  QObject::connect(&*songs_request_, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::SongsResultsReceived);
  QObject::connect(&*songs_request_, &YouTubeMusicRequest::PageReady, this, [this](const int, const SongMap &songs) {
    if (!songs.isEmpty()) Q_EMIT SongsResults(songs, QString());
  });
  QObject::connect(&*songs_request_, &YouTubeMusicRequest::UpdateStatus, this, &YouTubeMusicService::SongsUpdateStatusReceived);

  songs_request_->Process(YouTubeMusicRequest::Type::LibrarySongs);

}


void YouTubeMusicService::IndexLibrary(const int max_artists) {

  if (index_task_id_ >= 0) return;   // already running

  // Artists to walk: everything in the library whose index entry is missing or older than the
  // TTL, oldest first so repeated runs make progress.
  struct Candidate { QString id; QString name; QString stamp; };
  QList<Candidate> candidates;
  {
    QMutexLocker locker(database_->Mutex());
    QSqlDatabase db = database_->Connect();
    SqlQuery q(db);
    q.prepare(QStringLiteral("SELECT DISTINCT artist_id, artist FROM youtubemusic_artists_songs "
                             "WHERE artist_id <> '' AND artist <> ''"));
    if (!q.Exec()) {
      database_->ReportErrors(q);
      return;
    }
    while (q.next()) candidates << Candidate{q.value(0).toString(), q.value(1).toString(), QString()};
  }

  Settings s;
  s.beginGroup(YouTubeMusicSettings::kSettingsGroup);
  const QVariantMap indexed = s.value(YouTubeMusicSettings::kIndexedArtists).toMap();
  s.endGroup();

  const QDateTime now = QDateTime::currentDateTime();
  QList<Candidate> stale;
  for (const Candidate &candidate : std::as_const(candidates)) {
    const QDateTime stamp = QDateTime::fromString(indexed.value(candidate.id).toString(), Qt::ISODate);
    if (stamp.isValid() && stamp.secsTo(now) < YouTubeMusicSettings::kIndexTtlDays * 24 * 60 * 60) continue;
    stale << candidate;
  }
  std::sort(stale.begin(), stale.end(), [&indexed](const Candidate &a, const Candidate &b) {
    return indexed.value(a.id).toString() < indexed.value(b.id).toString();
  });

  if (max_artists > 0 && stale.count() > max_artists) stale = stale.mid(0, max_artists);

  QStringList ids;
  QStringList names;
  for (const Candidate &candidate : std::as_const(stale)) {
    ids << candidate.id;
    names << candidate.name;
  }

  BeginIndex(ids, names);

}

void YouTubeMusicService::BeginIndex(const QStringList &artist_ids, const QStringList &artist_names) {

  index_artist_ids_ = artist_ids;
  index_artist_names_ = artist_names;

  // The prefetcher only knows the id; without the name, tracks that arrive without an artist
  // would be stored with an empty one.
  for (int i = 0; i < index_artist_names_.count(); ++i) {
    if (!index_artist_names_.at(i).isEmpty()) continue;
    QMutexLocker locker(database_->Mutex());
    QSqlDatabase db = database_->Connect();
    SqlQuery q(db);
    q.prepare(QStringLiteral("SELECT artist FROM youtubemusic_artists_songs "
                             "WHERE artist_id = ? AND artist <> '' AND artist NOT LIKE '%-MPREb%' AND artist NOT LIKE '%-MPLAU%' "
                             "ORDER BY length(artist) LIMIT 1"));
    q.addBindValue(index_artist_ids_.value(i));
    if (q.Exec() && q.next()) index_artist_names_[i] = q.value(0).toString();
  }
  index_cursor_ = 0;
  index_cancelled_ = false;

  if (index_artist_ids_.isEmpty()) {
    qLog(Info) << "YouTubeMusic: nothing to index, every artist is fresh";
    Q_EMIT SongsUpdateStatus(tr("Library index is up to date."));
    return;
  }

  index_task_id_ = task_manager_->StartTask(tr("Indexing YouTube Music library"));
  qLog(Info) << "YouTubeMusic: indexing" << index_artist_ids_.count() << "artist(s)";
  StartNextIndexArtist();

}

void YouTubeMusicService::PrefetchNextArtist() {

  if (!prefetch_ || index_task_id_ >= 0) return;

  // Only when nothing else is going on: no running downloads.
  for (const DownloadEntry &entry : std::as_const(download_entries_)) {
    if (entry.state == DownloadEntry::State::Queued || entry.state == DownloadEntry::State::Downloading) return;
  }

  Settings s;
  s.beginGroup(YouTubeMusicSettings::kSettingsGroup);
  const QVariantMap indexed = s.value(YouTubeMusicSettings::kIndexedArtists).toMap();
  const QStringList recent = s.value(YouTubeMusicSettings::kRecentArtists).toStringList();
  s.endGroup();

  const QDateTime now = QDateTime::currentDateTime();
  auto is_stale = [&indexed, &now](const QString &id) {
    const QDateTime stamp = QDateTime::fromString(indexed.value(id).toString(), Qt::ISODate);
    return !stamp.isValid() || stamp.secsTo(now) >= YouTubeMusicSettings::kIndexTtlDays * 24 * 60 * 60;
  };

  // Preferred: what the user just looked at.
  for (const QString &id : recent) {
    if (!is_stale(id)) continue;
    qLog(Info) << "YouTubeMusic: prefetching recently browsed artist" << id;
    BeginIndex({id}, {QString()});
    return;
  }

  // Otherwise the oldest entry, so a background run eventually covers the library.
  QString oldest_id;
  QString oldest_stamp;
  QMutexLocker locker(database_->Mutex());
  QSqlDatabase db = database_->Connect();
  SqlQuery q(db);
  q.prepare(QStringLiteral("SELECT DISTINCT artist_id, artist FROM youtubemusic_artists_songs WHERE artist_id <> ''"));
  if (!q.Exec()) return;
  while (q.next()) {
    const QString id = q.value(0).toString();
    if (!is_stale(id)) continue;
    const QString stamp = indexed.value(id).toString();
    if (oldest_id.isEmpty() || stamp < oldest_stamp) {
      oldest_id = id;
      oldest_stamp = stamp;
    }
  }
  locker.unlock();

  if (oldest_id.isEmpty()) return;

  qLog(Info) << "YouTubeMusic: prefetching artist" << oldest_id;
  BeginIndex({oldest_id}, {QString()});

}

void YouTubeMusicService::RememberBrowsedArtist(const Song &artist) {

  if (artist.artist_id().isEmpty()) return;

  Settings s;
  s.beginGroup(YouTubeMusicSettings::kSettingsGroup);
  QStringList recent = s.value(YouTubeMusicSettings::kRecentArtists).toStringList();
  recent.removeAll(artist.artist_id());
  recent.prepend(artist.artist_id());
  while (recent.count() > 20) recent.removeLast();
  s.setValue(YouTubeMusicSettings::kRecentArtists, recent);
  s.endGroup();
  s.sync();

}

void YouTubeMusicService::CancelIndexLibrary() {

  if (index_task_id_ < 0) return;
  index_cancelled_ = true;

}

void YouTubeMusicService::StartNextIndexArtist() {

  for (YouTubeMusicRequest *request : std::as_const(index_requests_)) {
    if (request) request->deleteLater();
  }
  index_requests_.clear();
  index_albums_.clear();
  index_next_album_ = 0;

  if (index_cancelled_ || index_cursor_ >= index_artist_ids_.count()) {
    FinishIndexLibrary();
    return;
  }

  const QString artist_id = index_artist_ids_.at(index_cursor_);
  const QString artist_name = index_artist_names_.at(index_cursor_);
  index_artist_had_errors_ = false;

  task_manager_->SetTaskProgress(index_task_id_, index_cursor_, index_artist_ids_.count());
  Q_EMIT SongsUpdateStatus(tr("Indexing %1 (%2/%3)...").arg(artist_name).arg(index_cursor_ + 1).arg(index_artist_ids_.count()));

  const Song artist = ContainerSong(artist_id, artist_name, QString(), true);
  auto *request = new YouTubeMusicRequest(this, network_, this);
  index_requests_.insert(0, request);
  QObject::connect(request, &YouTubeMusicRequest::Results, this, &YouTubeMusicService::IndexArtistAlbumsReceived);
  request->ProcessArtistAlbums(artist);

}

void YouTubeMusicService::IndexArtistAlbumsReceived(const int id, const SongMap &songs, const QString &error) {

  Q_UNUSED(id);

  if (YouTubeMusicRequest *request = index_requests_.take(0)) request->deleteLater();

  if (!error.isEmpty()) {
    index_artist_had_errors_ = true;
    qLog(Warning) << "YouTubeMusic: index could not read albums of" << index_artist_names_.value(index_cursor_) << ":" << error;
  }

  for (const Song &album : songs) {
    if (!album.album_id().isEmpty()) index_albums_ << album;
  }

  index_albums_total_ = index_albums_.count();
  index_albums_done_ = 0;
  StartNextIndexAlbum();

}

void YouTubeMusicService::StartNextIndexAlbum() {

  if (index_cancelled_) {
    FinishIndexLibrary();
    return;
  }

  // Three albums in flight, like the artist walk.
  constexpr int kMaxConcurrentAlbums = 3;
  while (index_requests_.size() < kMaxConcurrentAlbums && index_next_album_ < index_albums_.count()) {
    const int slot = index_next_album_++;
    const Song album = index_albums_.at(slot);
    auto *request = new YouTubeMusicRequest(this, network_, this);
    index_requests_.insert(slot, request);
    // Capture the slot: the reply has to retire *its own* request, not the lowest key.
    QObject::connect(request, &YouTubeMusicRequest::Results, this,
                     [this, slot](const int, const SongMap &songs, const QString &error) {
                       IndexAlbumTracksReceived(slot, songs, error);
                     });
    request->ProcessAlbum(album);
  }

  if (index_requests_.isEmpty()) {
    FinishIndexAlbum();
  }

}

void YouTubeMusicService::IndexAlbumTracksReceived(const int slot, const SongMap &songs, const QString &error) {

  if (!error.isEmpty()) {
    index_artist_had_errors_ = true;
    qLog(Warning) << "YouTubeMusic: index could not read an album of"
                  << index_artist_names_.value(index_cursor_) << ":" << error;
  }

  if (YouTubeMusicRequest *request = index_requests_.take(slot)) request->deleteLater();

  if (!songs.isEmpty()) {
    SongList tracks = songs.values();
    // An album page lists its tracks with title + duration only: the album context has to come
    // from the album row this walk used, exactly like the expansion path does. Without it the
    // tracks landed with no album and the album row stayed empty.
    const Song album = index_albums_.value(slot);
    // The artist is the one whose walk produced this album, not the album's credit line:
    // "Rezz & PVRIS" is an album credit, and using it split the tree into 1264 artists
    // (769 of them collaboration strings) instead of the 145 in the library.
    const QString walked_artist = index_artist_names_.value(index_cursor_);
    for (Song &track : tracks) {
      if (track.album().isEmpty() && !album.album().isEmpty()) track.set_album(album.album());
      if (track.album_id().isEmpty() && !album.album_id().isEmpty()) track.set_album_id(album.album_id());
      const QString artist = walked_artist.isEmpty() ? album.artist() : walked_artist;
      if (!artist.isEmpty()) {
        track.set_artist(artist);
        track.set_albumsort(track.album());
        track.set_albumartist(artist);
      }
    }
    // Only the artists tree stores artist -> album -> track; the albums list holds album rows.
    artists_collection_backend_->AddOrUpdateSongsAsync(tracks);
  }

  ++index_albums_done_;
  if (index_albums_total_ > 0) {
    task_manager_->SetTaskProgress(index_task_id_, index_cursor_, index_artist_ids_.count());
  }
  StartNextIndexAlbum();

}

void YouTubeMusicService::FinishIndexAlbum() {

  // An artist whose albums could not be read is *not* stamped: the next run (button or the
  // prefetcher) retries it instead of leaving it broken for the whole TTL.
  if (index_artist_had_errors_) {
    qLog(Warning) << "YouTubeMusic: leaving" << index_artist_names_.value(index_cursor_)
                  << "unstamped because some of its albums failed";
    ++index_cursor_;
    StartNextIndexArtist();
    return;
  }

  Settings s;
  s.beginGroup(YouTubeMusicSettings::kSettingsGroup);
  QVariantMap indexed = s.value(YouTubeMusicSettings::kIndexedArtists).toMap();
  indexed.insert(index_artist_ids_.value(index_cursor_), QDateTime::currentDateTime().toString(Qt::ISODate));
  s.setValue(YouTubeMusicSettings::kIndexedArtists, indexed);
  s.endGroup();
  s.sync();

  qLog(Info) << "YouTubeMusic: indexed" << index_artist_names_.value(index_cursor_)
             << "(" << index_albums_total_ << "albums )";

  ++index_cursor_;
  StartNextIndexArtist();

}

void YouTubeMusicService::FinishIndexLibrary(const QString &error) {

  for (YouTubeMusicRequest *request : std::as_const(index_requests_)) {
    if (request) request->deleteLater();
  }
  index_requests_.clear();

  if (index_task_id_ >= 0) {
    task_manager_->SetTaskFinished(index_task_id_);
    index_task_id_ = -1;
  }

  index_artist_ids_.clear();
  index_artist_names_.clear();
  index_albums_.clear();
  index_next_album_ = 0;

  Q_EMIT SongsUpdateStatus(error.isEmpty() ? QString() : error);
  qLog(Info) << "YouTubeMusic: library index finished";

}

void YouTubeMusicService::LoadMoreArtists() {

  // The request layer defaults to 20 library pages, so "more" has to start above that.
  page_budget_ = (page_budget_ > 0 ? page_budget_ : 20) + 10;
  qLog(Info) << "YouTubeMusic: loading more artists (page budget" << page_budget_ << ")";
  GetArtists();

}

void YouTubeMusicService::LoadMoreAlbums() {

  page_budget_ = (page_budget_ > 0 ? page_budget_ : 20) + 10;
  qLog(Info) << "YouTubeMusic: loading more albums (page budget" << page_budget_ << ")";
  GetAlbums();

}

void YouTubeMusicService::LoadMoreSongs() {

  page_budget_ = (page_budget_ > 0 ? page_budget_ : 20) + 10;
  qLog(Info) << "YouTubeMusic: loading more songs (page budget" << page_budget_ << ")";
  GetSongs();

}

void YouTubeMusicService::LoadMoreSearch(const QString &query) {

  page_budget_ = (page_budget_ > 0 ? page_budget_ : 5) + 5;
  qLog(Info) << "YouTubeMusic: loading more search results for" << query;
  Q_EMIT Search(query, YouTubeMusicService::SearchType::Songs);

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


void YouTubeMusicService::MergeCollaborationArtists() {

  // The liked-artists shelf is the subscription list. YouTube Music also exposes collaboration
  // *entities* ("The Chainsmokers & Coldplay", "Berg & Vermont (BR)"), which used to appear as
  // extra artists. A collaboration is folded into a subscribed artist only when one of its parts
  // is itself subscribed, so real band names ("Chase & Status", "Of Mice & Men") stay untouched.
  QHash<QString, QString> subscribed;   // lower-case name -> canonical name
  {
    QMutexLocker locker(database_->Mutex());
    QSqlDatabase db = database_->Connect();
    SqlQuery q(db);
    // The liked-artists shelf is stored as rows without an album or a song; the songs table
    // only lists artists of liked songs, which is not the subscription list.
    q.prepare(QStringLiteral("SELECT DISTINCT artist FROM youtubemusic_artists_songs "
                             "WHERE coalesce(album_id, '') = '' AND coalesce(song_id, '') = '' AND artist <> ''"));
    if (!q.Exec()) {
      database_->ReportErrors(q);
      return;
    }
    while (q.next()) {
      const QString name = q.value(0).toString().trimmed();
      if (!name.isEmpty() && !subscribed.contains(name.toLower())) subscribed.insert(name.toLower(), name);
    }
  }

  if (subscribed.isEmpty()) return;

  QHash<QString, QString> merges;   // collaboration name -> subscribed artist
  {
    QMutexLocker locker(database_->Mutex());
    QSqlDatabase db = database_->Connect();
    SqlQuery q(db);
    q.prepare(QStringLiteral("SELECT DISTINCT artist FROM youtubemusic_artists_songs WHERE artist <> ''"));
    if (!q.Exec()) return;
    QStringList names;
    while (q.next()) names << q.value(0).toString();
    locker.unlock();

    static const QRegularExpression split_re(uR"re(\s*(?:&|\bfeat\.?\b|\bx\b|,)\s*)re"_s, QRegularExpression::CaseInsensitiveOption);
    for (const QString &name : std::as_const(names)) {
      if (!split_re.match(name).hasMatch()) continue;
      if (subscribed.contains(name.toLower())) continue;   // itself subscribed - leave alone
      // Prefer the longest subscribed name contained in this one: splitting would break a
      // nested collaboration ("Becky Hill & Chase & Status" -> "Chase", "Status") and the row
      // would then be dropped instead of folded.
      QString best;
      for (auto sub = subscribed.cbegin(); sub != subscribed.cend(); ++sub) {
        if (sub.key().length() < 3) continue;
        if (!name.contains(sub.key(), Qt::CaseInsensitive)) continue;
        if (best.length() < sub.key().length()) best = sub.key();
      }
      if (!best.isEmpty()) {
        merges.insert(name, subscribed.value(best));
        continue;
      }

      const QStringList parts = name.split(split_re, Qt::SkipEmptyParts);
      for (const QString &part : parts) {
        const QString candidate = part.trimmed();
        const auto it = subscribed.constFind(candidate.toLower());
        if (it == subscribed.cend()) continue;
        merges.insert(name, it.value());
        break;
      }
    }
  }

  if (merges.isEmpty()) {
    qLog(Info) << "YouTubeMusic: no collaboration artists to fold";
  }

  QMutexLocker locker(database_->Mutex());
  QSqlDatabase db = database_->Connect();
  for (auto it = merges.cbegin(); it != merges.cend(); ++it) {
    SqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE youtubemusic_artists_songs SET artist = ? WHERE artist = ?"));
    q.addBindValue(it.value());
    q.addBindValue(it.key());
    if (!q.Exec()) database_->ReportErrors(q);
  }

  qLog(Info) << "YouTubeMusic: folded" << merges.count() << "collaboration artist(s) into subscribed artists";

  // Strict: the artist browser shows the subscription list, so rows that ended up under an
  // artist the user does not follow (album credits such as "Samantha Machado" on a compilation)
  // are dropped from the tree. They stay reachable from the Albums and Songs tabs.
  QHash<QString, QString> subscribed_after;
  {
    SqlQuery q(db);
    q.prepare(QStringLiteral("SELECT DISTINCT artist FROM youtubemusic_artists_songs "
                             "WHERE coalesce(album_id, '') = '' AND coalesce(song_id, '') = '' AND artist <> ''"));
    if (!q.Exec()) {
      database_->ReportErrors(q);
      return;
    }
    while (q.next()) subscribed_after.insert(q.value(0).toString(), q.value(0).toString());
  }

  // Guard: with no subscription list the DELETE below matches every row in the tree. That
  // happens on a fresh profile where indexing ran before the artists shelf was fetched, or when
  // that fetch failed - so refuse rather than wipe the table.
  if (subscribed_after.isEmpty()) {
    qLog(Warning) << "YouTubeMusic: no subscription list available, skipping the strict cleanup";
    return;
  }

  SqlQuery stray(db);
  stray.prepare(QStringLiteral("SELECT DISTINCT artist FROM youtubemusic_artists_songs "
                               "WHERE artist <> '' AND artist NOT IN (SELECT artist FROM youtubemusic_artists_songs "
                               "WHERE coalesce(album_id, '') = '' AND coalesce(song_id, '') = '' AND artist <> '')"));
  if (!stray.Exec()) {
    database_->ReportErrors(stray);
    return;
  }
  QStringList strays;
  while (stray.next()) strays << stray.value(0).toString();

  // Defence in depth: if more than half of the names look stray, something is wrong with the
  // subscription list; leave the tree alone and say so.
  SqlQuery total_q(db);
  total_q.prepare(QStringLiteral("SELECT count(DISTINCT artist) FROM youtubemusic_artists_songs"));
  int total_artists = 0;
  if (total_q.Exec() && total_q.next()) total_artists = total_q.value(0).toInt();
  if (total_artists > 0 && strays.count() * 2 > total_artists) {
    qLog(Warning) << "YouTubeMusic: refusing to drop" << strays.count() << "of" << total_artists
                  << "artists - the subscription list looks incomplete";
    return;
  }

  for (const QString &artist : std::as_const(strays)) {
    SqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM youtubemusic_artists_songs WHERE artist = ?"));
    q.addBindValue(artist);
    if (!q.Exec()) {
      database_->ReportErrors(q);
      return;
    }
  }

  if (!strays.isEmpty()) {
    qLog(Info) << "YouTubeMusic: removed" << strays.count() << "artist(s) that are not subscribed:" << strays.join(u", "_s);
  }

  // An album row whose pages never yielded a track opens to nothing: drop it from the tree.
  // If a later walk finds tracks for it, the row comes back with them.
  SqlQuery empty(db);
  empty.prepare(QStringLiteral("DELETE FROM youtubemusic_artists_songs WHERE album_id <> '' AND album_id IN ("
                               "SELECT album_id FROM youtubemusic_artists_songs WHERE album_id <> '' "
                               "GROUP BY album_id HAVING sum(CASE WHEN coalesce(song_id, '') <> '' THEN 1 ELSE 0 END) = 0)"));
  if (!empty.Exec()) {
    database_->ReportErrors(empty);
    return;
  }
  const int empty_albums = empty.numRowsAffected();
  if (empty_albums > 0) {
    qLog(Info) << "YouTubeMusic: removed" << empty_albums << "album row(s) that have no tracks";
  }

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
    download_container_is_artist_ = false;
    Q_EMIT SongsUpdateStatus(tr("Fetching tracks of %1 for download...").arg(container.album().isEmpty() ? container.album_id() : container.album()));
    download_container_request_->ProcessAlbum(container);
  }
  else {
    // An artist row means the whole discography: list the albums first, then each album is
    // queued as its own container so its tracks are fetched and downloaded.
    download_container_is_artist_ = true;
    Q_EMIT SongsUpdateStatus(tr("Fetching albums of %1 for download...").arg(container.artist()));
    download_container_request_->ProcessArtistAlbums(container);
  }

}

void YouTubeMusicService::DownloadContainerTracksReceived(const int id, const SongMap &songs, const QString &error) {

  Q_UNUSED(id);

  if (error.isEmpty() && download_container_is_artist_) {
    // The reply is the artist's album list: queue them (in reverse, so takeFirst() walks
    // them in order) and let the album path fetch the tracks.
    int queued = 0;
    for (const Song &album : songs) {
      if (album.album_id().isEmpty()) continue;
      download_containers_.prepend(album);
      ++queued;
    }
    qLog(Info) << "YouTubeMusic: queued" << queued << "album(s) for download";
    Q_EMIT SongsUpdateStatus(tr("Queued %1 album(s) for download, fetching their tracks...").arg(queued));
  }
  else if (error.isEmpty()) {
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
  artist_walk_requests_.clear();
  artist_walk_results_.clear();
  artist_walk_next_ = 0;

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

  artist_walk_results_.resize(artist_tracks_albums_.count());
  qLog(Info) << "YouTubeMusic: collecting tracks from" << artist_tracks_albums_.count() << "album(s)";
  StartNextArtistAlbumTracks();

}

void YouTubeMusicService::StartNextArtistAlbumTracks() {

  // Three albums in flight at a time: fast enough for a 25 album artist to feel instant,
  // and still polite to the API. Results are stored per album so the order is preserved.
  constexpr int kMaxConcurrentAlbums = 3;

  while (artist_walk_requests_.size() < kMaxConcurrentAlbums && artist_walk_next_ < artist_tracks_albums_.count()) {
    const int index = artist_walk_next_++;
    const Song album = artist_tracks_albums_.at(index);

    auto *request = new YouTubeMusicRequest(this, network_, this);
    artist_walk_requests_.insert(index, request);
    QObject::connect(request, &YouTubeMusicRequest::Results, this,
                     [this, index](const int id, const SongMap &songs, const QString &error) {
                       Q_UNUSED(id);
                       if (YouTubeMusicRequest *finished = artist_walk_requests_.take(index)) finished->deleteLater();
                       if (error.isEmpty()) {
                         SongList collected;
                         for (const Song &song : songs) {
                           if (!song.song_id().isEmpty()) collected << song;
                         }
                         artist_walk_results_[index] = collected;
                       }
                       StartNextArtistAlbumTracks();
                     });
    request->ProcessAlbum(album);
  }

  const int remaining = artist_tracks_albums_.count() - artist_walk_next_ + artist_walk_requests_.size();
  Q_EMIT SongsUpdateStatus(tr("Collecting tracks: %1 album(s) left").arg(std::max(0, remaining)));

  if (artist_walk_requests_.isEmpty() && artist_walk_next_ >= artist_tracks_albums_.count()) {
    FinishArtistTrackWalk();
  }

}

void YouTubeMusicService::FinishArtistTrackWalk() {

  for (YouTubeMusicRequest *request : std::as_const(artist_walk_requests_)) {
    if (request) request->deleteLater();
  }
  artist_walk_requests_.clear();

  artist_tracks_collected_.clear();
  for (const SongList &album_tracks : artist_walk_results_) {
    for (const Song &song : album_tracks) {
      const bool duplicate = std::any_of(artist_tracks_collected_.cbegin(), artist_tracks_collected_.cend(),
                                         [&song](const Song &existing) { return existing.song_id() == song.song_id(); });
      if (!duplicate) artist_tracks_collected_ << song;
    }
  }
  artist_walk_results_.clear();
  artist_walk_next_ = 0;

  qLog(Info) << "YouTubeMusic: collected" << artist_tracks_collected_.count() << "track(s) for the artist";

  Q_EMIT SongsUpdateStatus(QString());
  Q_EMIT AlbumTracksLoaded(artist_tracks_collected_, QString());

}

void YouTubeMusicService::ExpandArtistRow(const Song &artist) {

  if (artist.artist_id().isEmpty()) return;
  if (expanded_artists_.contains(artist.artist_id())) return;
  expanded_artists_.insert(artist.artist_id());

  RememberBrowsedArtist(artist);

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
      QString parent_artist = expand_album_context_.artist();
      // The parent name can be the model's composite container key ("Stromae-QUE CE SOIT
      // CLAIR-MPREb_..."): the artist is its first segment, never the whole string.
      if (parent_artist.contains(u"-MPREb"_s) || parent_artist.contains(u"-MPLAU"_s)) {
        parent_artist = parent_artist.section(u'-', 0, 0).trimmed();
      }
      if (!parent_artist.isEmpty()) {
        for (Song &track : tracks) {
          track.set_artist(parent_artist);
          track.set_albumartist(parent_artist);
        }
      }
      // Single-track albums used to be flattened here (album level dropped and the album's
      // descriptor row deleted), which made the album *and* its track disappear from where the
      // user was looking - 950 albums in this library matched. Rows stay as they are now.
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
