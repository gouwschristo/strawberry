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
#include <QStringList>
#include <QUrl>
#include <QList>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkReply>

#include "includes/shared_ptr.h"
#include "core/logging.h"
#include "core/song.h"
#include "youtubemusic/youtubemusicservice.h"
#include "youtubemusicrequest.h"

using namespace Qt::Literals::StringLiterals;

namespace {

// Search filters (base64 encoded protobuf), the same values the web client sends.
constexpr char kParamsArtists[] = "EgWKAQIgAWoKEAkQChAFEAMQBA==";
constexpr char kParamsAlbums[] = "EgWKAQIYAWoKEAkQChAFEAMQBA==";
constexpr char kParamsSongs[] = "EgWKAQIIAWoKEAkQBRAKEAMQBA==";

// Artists you have tracks from. "FEmusic_liked_artists" answers HTTP 400
// INVALID_ARGUMENT, this is the endpoint the library UI links to.
constexpr char kBrowseLikedArtists[] = "FEmusic_library_corpus_track_artists";
constexpr char kBrowseLikedAlbums[] = "FEmusic_liked_albums";
constexpr char kBrowseLikedSongs[] = "FEmusic_liked_videos";

// Safety caps: a library shelf page holds 12-25 rows.
constexpr int kMaxLibraryPages = 20;
constexpr int kMaxSearchPages = 5;

// Every item in a list or grid: musicResponsiveListItemRenderer (list rows) and
// musicTwoRowItemRenderer (grid tiles) are the two shapes the web client uses.
void CollectItems(const QJsonValue &value, QList<QJsonObject> &items, const bool list_rows_only = false) {

  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    if (object.contains("musicResponsiveListItemRenderer"_L1)) {
      items << object["musicResponsiveListItemRenderer"_L1].toObject();
      return;
    }
    if (object.contains("musicTwoRowItemRenderer"_L1)) {
      if (!list_rows_only) items << object["musicTwoRowItemRenderer"_L1].toObject();
      return;
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
      CollectItems(it.value(), items, list_rows_only);
    }
  }
  else if (value.isArray()) {
    const QJsonArray array = value.toArray();
    for (const QJsonValue &item : array) {
      CollectItems(item, items, list_rows_only);
    }
  }

}

// Text object: either {"simpleText": "..."} or {"runs": [{"text": "..."}, ...]}.
QStringList TextRuns(const QJsonValue &value) {

  QStringList ret;

  if (!value.isObject()) return ret;

  const QJsonObject object = value.toObject();
  if (object.contains("simpleText"_L1)) {
    ret << object["simpleText"_L1].toString();
    return ret;
  }

  const QJsonValue runs = object["runs"_L1];
  if (runs.isArray()) {
    const QJsonArray array = runs.toArray();
    for (const QJsonValue &run : array) {
      ret << run.toObject()["text"_L1].toString();
    }
  }

  return ret;

}

void CollectObjectsWithKey(const QJsonValue &value, const QString &key, QList<QJsonObject> &objects);

// Album pages put "Album • 2004" in the header subtitle - except when that slot carries the
// label instead ("Album • Alpha Portal"), so only a four digit run counts as a year.
int AlbumYearFromHeader(const QJsonObject &json_object) {

  static const QRegularExpression year_re(u"^\\d{4}$"_s);

  QList<QJsonObject> headers;
  CollectObjectsWithKey(json_object, u"musicResponsiveHeaderRenderer"_s, headers);
  for (const QJsonObject &header : std::as_const(headers)) {
    const QJsonArray runs = header[u"subtitle"_s].toObject()[u"runs"_s].toArray();
    for (const QJsonValue &run : runs) {
      const QString text = run.toObject()[u"text"_s].toString().trimmed();
      if (!year_re.match(text).hasMatch()) continue;
      const int year = text.toInt();
      if (year > 1000) return year;
    }
  }

  return -1;

}

void CollectObjectsWithKey(const QJsonValue &value, const QString &key, QList<QJsonObject> &objects) {

  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    const QJsonValue found = object[key];
    if (found.isObject()) {
      objects << found.toObject();
      return;
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
      CollectObjectsWithKey(it.value(), key, objects);
    }
  }
  else if (value.isArray()) {
    const QJsonArray array = value.toArray();
    for (const QJsonValue &item : array) {
      CollectObjectsWithKey(item, key, objects);
    }
  }

}

// The largest thumbnail in any "thumbnails" array below the value.
QUrl LargestThumbnail(const QJsonValue &value) {

  QList<QJsonObject> arrays;
  CollectObjectsWithKey(value, u"thumbnail"_s, arrays);

  QUrl ret;
  int largest_size = 0;

  for (const QJsonObject &thumbnail_renderer : arrays) {
    const QJsonArray thumbnails = thumbnail_renderer["thumbnails"_L1].toArray();
    for (const QJsonValue &thumbnail_value : thumbnails) {
      const QJsonObject thumbnail = thumbnail_value.toObject();
      const int size = thumbnail["width"_L1].toInt() * thumbnail["height"_L1].toInt();
      const QString url = thumbnail["url"_L1].toString();
      if (url.isEmpty()) continue;
      // The web client appends "=w120-h120"; ask for a decent size instead.
      QString sized_url = url;
      const qsizetype strip = sized_url.lastIndexOf(u'=');
      if (strip > 0 && sized_url.mid(strip).contains(u"-h"_s)) sized_url = sized_url.left(strip);
      sized_url += u"=w600-h600"_s;
      if (size >= largest_size) {
        largest_size = size;
        ret = QUrl(sized_url);
      }
    }
  }

  return ret;

}

QString PageType(const QJsonObject &browse_endpoint) {

  const QJsonObject configs = browse_endpoint["browseEndpointContextSupportedConfigs"_L1].toObject();
  const QJsonObject music_config = configs["browseEndpointContextMusicConfig"_L1].toObject();

  return music_config["pageType"_L1].toString();

}

// The browse id matching one of the wanted page types, falling back to the first one.
QString BrowseId(const QJsonValue &item, const QStringList &wanted_page_types, const bool strict = false) {

  QList<QJsonObject> browse_endpoints;
  CollectObjectsWithKey(item, u"browseEndpoint"_s, browse_endpoints);

  QString fallback;
  for (const QJsonObject &browse_endpoint : browse_endpoints) {
    const QString browse_id = browse_endpoint["browseId"_L1].toString();
    if (browse_id.isEmpty()) continue;
    if (wanted_page_types.contains(PageType(browse_endpoint))) return browse_id;
    if (fallback.isEmpty()) fallback = browse_id;
  }

  return strict ? QString() : fallback;

}

QString VideoId(const QJsonValue &item) {

  QList<QJsonObject> watch_endpoints;
  CollectObjectsWithKey(item, u"watchEndpoint"_s, watch_endpoints);

  for (const QJsonObject &watch_endpoint : watch_endpoints) {
    const QString video_id = watch_endpoint["videoId"_L1].toString();
    if (!video_id.isEmpty()) return video_id;
  }

  return QString();

}

qint64 DurationFromText(const QString &text) {

  const QStringList parts = text.split(u':');
  if (parts.isEmpty() || parts.count() > 3) return 0;

  qint64 seconds = 0;
  for (const QString &part : parts) {
    bool ok = false;
    const int value = part.toInt(&ok);
    if (!ok) return 0;
    seconds = (seconds * 60) + value;
  }

  return seconds * 1000000000LL;

}

// The first subtitle segment of library rows is the item type, e.g.
// "Single • Osax • 2024" or "Album • Romulus • 2012".
void StripItemType(QStringList &segments) {

  static const QStringList types = {u"Single"_s, u"Album"_s, u"EP"_s, u"Live"_s, u"Compilation"_s, u"Song"_s, u"Video"_s};

  if (!segments.isEmpty() && types.contains(segments.first())) segments.removeFirst();

}

// InnerTube reports the session state in responseContext.serviceTrackingParams:
// {"service": "GFEEDBACK", "params": [{"key": "logged_in", "value": "0"}]}.
// An expired cookie usually produces HTTP 200 with this flag, not a 401.
bool IsLoggedOut(const QJsonValue &value) {

  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    const QJsonArray params = object["params"_L1].toArray();
    for (const QJsonValue &param_value : params) {
      const QJsonObject param = param_value.toObject();
      if (param["key"_L1].toString() == u"logged_in"_s) {
        return param["value"_L1].toString() == u"0"_s;
      }
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
      if (IsLoggedOut(it.value())) return true;
    }
  }
  else if (value.isArray()) {
    const QJsonArray array = value.toArray();
    for (const QJsonValue &item : array) {
      if (IsLoggedOut(item)) return true;
    }
  }

  return false;

}

bool LooksLikeDuration(const QString &text) {

  if (text.isEmpty()) return false;

  const QStringList parts = text.split(u':');
  if (parts.count() == 0 || parts.count() > 3) return false;

  for (const QString &part : parts) {
    bool ok = false;
    part.toInt(&ok);
    if (!ok) return false;
  }

  return true;

}

// The " • " separated line below the title, e.g. "Artist • Album • 3:44".
QStringList Subtitles(const QJsonObject &item) {

  QStringList columns;

  const QJsonValue flex_columns = item["flexColumns"_L1];
  if (flex_columns.isArray()) {
    const QJsonArray array = flex_columns.toArray();
    for (const QJsonValue &column : array) {
      const QJsonObject renderer = column.toObject()["musicResponsiveListItemFlexColumnRenderer"_L1].toObject();
      columns << TextRuns(renderer["text"_L1]).join(QString());
    }
  }

  if (columns.isEmpty()) {
    // musicTwoRowItemRenderer
    columns << TextRuns(item["title"_L1]).join(QString());
    columns << TextRuns(item["subtitle"_L1]).join(QString());
  }

  QStringList ret;
  if (columns.count() < 2) return ret;

  ret = columns[1].split(u" • "_s, Qt::SkipEmptyParts);
  for (QString &value : ret) value = value.trimmed();

  return ret;

}

}  // namespace

YouTubeMusicRequest::YouTubeMusicRequest(YouTubeMusicService *service, const SharedPtr<NetworkAccessManager> network, QObject *parent)
    : YouTubeMusicBaseRequest(service, network, parent),
      type_(Type::None),
      search_id_(0),
      pages_(0),
      retried_auth_(false) {}

void YouTubeMusicRequest::Process(const Type type, const int search_id, const QString &query) {

  type_ = type;
  search_id_ = search_id;
  query_ = query;
  error_.clear();
  songs_.clear();
  pages_ = 0;

  const int limit = service_->search_limit();

  QJsonObject body;
  body["context"_L1] = CreateContext();

  QString endpoint;

  switch (type_) {

    case Type::SearchArtists:
    case Type::SearchAlbums:
    case Type::SearchSongs:{
      endpoint = u"/youtubei/v1/search"_s;
      body["query"_L1] = query_;
      if (type_ == Type::SearchArtists) body["params"_L1] = QLatin1String(kParamsArtists);
      else if (type_ == Type::SearchAlbums) body["params"_L1] = QLatin1String(kParamsAlbums);
      else body["params"_L1] = QLatin1String(kParamsSongs);
      Q_EMIT UpdateStatus(tr("Searching for %1...").arg(query_));
      break;
    }

    case Type::LibraryArtists:{
      endpoint = u"/youtubei/v1/browse"_s;
      body["browseId"_L1] = QLatin1String(kBrowseLikedArtists);
      Q_EMIT UpdateStatus(tr("Fetching artists from YouTube Music library..."));
      break;
    }

    case Type::LibraryAlbums:{
      endpoint = u"/youtubei/v1/browse"_s;
      body["browseId"_L1] = QLatin1String(kBrowseLikedAlbums);
      Q_EMIT UpdateStatus(tr("Fetching albums from YouTube Music library..."));
      break;
    }

    case Type::LibrarySongs:{
      endpoint = u"/youtubei/v1/browse"_s;
      body["browseId"_L1] = QLatin1String(kBrowseLikedSongs);
      Q_EMIT UpdateStatus(tr("Fetching songs from YouTube Music library..."));
      break;
    }

    case Type::None:{
      Q_EMIT Results(search_id_, SongMap(), tr("Invalid request."));
      return;
    }

  }

  Q_UNUSED(limit);

  Send(endpoint, body);

}

void YouTubeMusicRequest::ProcessAlbum(const Song &album) {

  type_ = Type::AlbumSongs;
  search_id_ = 0;
  album_source_ = album;
  error_.clear();
  songs_.clear();
  pages_ = 0;

  QJsonObject body;
  body["context"_L1] = CreateContext();
  body["browseId"_L1] = album.album_id();
  if (!album.album_id().isEmpty()) {
    Q_EMIT UpdateStatus(tr("Fetching tracks of %1...").arg(album.album().isEmpty() ? album.album_id() : album.album()));
  }

  Send(u"/youtubei/v1/browse"_s, body);

}

void YouTubeMusicRequest::ProcessArtist(const Song &artist) {

  type_ = Type::ArtistSongs;
  search_id_ = 0;
  artist_source_ = artist;
  resolved_artist_browse_id_.clear();
  error_.clear();
  songs_.clear();
  pages_ = 0;

  if (artist.artist_id().isEmpty()) {
    Q_EMIT Results(search_id_, SongMap(), tr("Artist has no id."));
    return;
  }

  Q_EMIT UpdateStatus(tr("Fetching tracks of %1...").arg(artist.artist()));

  QJsonObject body;
  body["context"_L1] = CreateContext();
  body["browseId"_L1] = artist.artist_id();

  Send(u"/youtubei/v1/browse"_s, body);

}

void YouTubeMusicRequest::ProcessArtistAlbums(const Song &artist) {

  type_ = Type::ArtistAlbums;
  search_id_ = 0;
  artist_source_ = artist;
  resolved_artist_browse_id_.clear();
  error_.clear();
  songs_.clear();
  pages_ = 0;

  if (artist.artist_id().isEmpty()) {
    Q_EMIT Results(search_id_, SongMap(), tr("Artist has no id."));
    return;
  }

  Q_EMIT UpdateStatus(tr("Fetching albums of %1...").arg(artist.artist()));

  QJsonObject body;
  body["context"_L1] = CreateContext();
  body["browseId"_L1] = artist.artist_id();

  Send(u"/youtubei/v1/browse"_s, body);

}

void YouTubeMusicRequest::ReplyFinished() {

  const JsonObjectResult result = ParseJsonObject(reply_);
  // The reply stays owned by HttpBaseRequest::replies_ (it aborts and deletes
  // them in its destructor). Deleting it here as well leaves a dangling pointer
  // in that list and crashes in AbortNetworkReplies() on shutdown.
  reply_ = nullptr;

  if (!result.success()) {
    // YouTube rotates the session cookies outside the API (responses carry no
    // Set-Cookie), so the only way to recover mid-session is to re-read them from
    // the browser. Do that once, then replay the request.
    if ((result.api_error == 401 || result.api_error == 403) && !retried_auth_ && service_->RefreshCookies()) {
      retried_auth_ = true;
      pages_ = 0;
      songs_.clear();
      resolved_artist_browse_id_.clear();
      qLog(Info) << "YouTubeMusic: session had expired, cookies refreshed - retrying request";
      Send(last_endpoint_, last_body_);
      return;
    }

    QString error = result.error_message.isEmpty() ? tr("Unknown error.") : result.error_message;
    // YouTube rotates the session cookies hourly; an expired cookie surfaces as a
    // 401/403 from InnerTube. Say what to do instead of echoing the API text.
    if (result.api_error == 401 || result.api_error == 403) {
      error = tr("YouTube Music session expired. Refresh the cookies in Settings > Streaming > YouTube Music (see strawberry-ytm-cookies.py).");
    }
    Q_EMIT Results(search_id_, SongMap(), error);
    return;
  }

  // A library artist id (MPLAUC...) opens a near-empty overview page; the real
  // artist page ("Top songs", albums) lives on the UC... channel id, which the
  // overview page links to.
  if ((type_ == Type::ArtistSongs || type_ == Type::ArtistAlbums) && resolved_artist_browse_id_.isEmpty() && result.json_object.size() > 0) {
    const QString artist_id = artist_source_.artist_id();
    if (artist_id.startsWith("MPLAUC"_L1)) {
      QList<QJsonObject> browse_endpoints;
      CollectObjectsWithKey(result.json_object, u"browseEndpoint"_s, browse_endpoints);
      for (const QJsonObject &browse_endpoint : browse_endpoints) {
        const QString browse_id = browse_endpoint["browseId"_L1].toString();
        if (browse_id.startsWith("UC"_L1) && PageType(browse_endpoint) == "MUSIC_PAGE_TYPE_ARTIST"_L1) {
          resolved_artist_browse_id_ = browse_id;
          break;
        }
      }
      if (!resolved_artist_browse_id_.isEmpty()) {
        QJsonObject body;
        body["context"_L1] = CreateContext();
        body["browseId"_L1] = resolved_artist_browse_id_;
        Send(u"/youtubei/v1/browse"_s, body);
        return;
      }
    }
  }

  if (!retried_auth_ && !service_->cookie().isEmpty() && IsLoggedOut(result.json_object) && service_->RefreshCookies()) {
    retried_auth_ = true;
    pages_ = 0;
    songs_.clear();
    resolved_artist_browse_id_.clear();
    qLog(Info) << "YouTubeMusic: session had expired (signed-out reply), cookies refreshed - retrying request";
    Send(last_endpoint_, last_body_);
    return;
  }

  ++pages_;
  ParseJson(result.json_object);

  // InnerTube pages its shelves: keep following the continuation token until the
  // list is exhausted (or the result limit for searches is reached), so the whole
  // library is pulled in instead of just the first 25/22/13 rows.
  const QString continuation = FindContinuation(result.json_object);
  const bool is_library = type_ == Type::LibraryArtists || type_ == Type::LibraryAlbums || type_ == Type::LibrarySongs;
  const int default_pages = is_library ? kMaxLibraryPages : kMaxSearchPages;
  const int max_pages = page_budget_ > 0 ? page_budget_ : default_pages;
  const bool paginate = type_ != Type::ArtistSongs && type_ != Type::ArtistAlbums;
  const bool limit_reached = !is_library && songs_.count() >= service_->search_limit();

  if (paginate && !continuation.isEmpty() && pages_ < max_pages && !limit_reached) {
    RequestContinuation(continuation);
    return;
  }

  EmitResults();

}

void YouTubeMusicRequest::ParseJson(const QJsonObject &json_object) {

  QList<QJsonObject> items;
  CollectItems(json_object, items, type_ == Type::AlbumSongs || type_ == Type::ArtistSongs);

  const int album_year = type_ == Type::AlbumSongs ? AlbumYearFromHeader(json_object) : -1;

  for (const QJsonObject &item : items) {

    Song song;

    switch (type_) {
      case Type::SearchSongs:
      case Type::LibrarySongs:
      case Type::AlbumSongs:
      case Type::ArtistSongs:
        song = ParseSongItem(item);
        break;
      case Type::ArtistAlbums:
        song = ParseAlbumItem(item, true);
        break;
      case Type::SearchArtists:
      case Type::LibraryArtists:
        song = ParseArtistItem(item);
        break;
      case Type::SearchAlbums:
      case Type::LibraryAlbums:
        song = ParseAlbumItem(item);
        break;
      case Type::None:
        break;
    }

    if (!song.is_valid()) continue;

    if (type_ == Type::ArtistAlbums) {
      if (song.album_id().isEmpty()) continue;
      if (song.albumartist().isEmpty()) song.set_albumartist(artist_source_.artist());
      if (song.artist().isEmpty()) song.set_artist(artist_source_.artist());
      if (!artist_source_.art_automatic().isValid()) song.set_art_automatic(artist_source_.art_automatic());
    }

    if (type_ == Type::ArtistSongs) {
      // Only rows with a video id are tracks; the page also carries album tiles
      // and "featured on" browse rows.
      if (song.song_id().isEmpty()) continue;
      if (song.artist().isEmpty()) song.set_artist(artist_source_.artist());
      if (!artist_source_.art_automatic().isValid()) song.set_art_automatic(artist_source_.art_automatic());
    }

    if (type_ == Type::AlbumSongs) {
      // Album pages also carry "related albums"/"you might like" shelves; only
      // rows with a video id are actual tracks of the album.
      if (song.song_id().isEmpty()) continue;
      // yt-dlp/InnerTube never expose a track number for these uploads, but an album
      // page lists its tracks in order, so number them here. Downloads use it for
      // the "NN - Title" filename.
      if (song.track() <= 0) song.set_track(static_cast<int>(songs_.count()) + 1);
      // The release year is a property of the album, not of the track row.
      if (album_year > 0 && song.year() <= 0) song.set_year(album_year);
      // Track rows on an album page only carry title + artist + duration, so fill
      // in the album context from the row the user activated.
      if (song.album().isEmpty() && !album_source_.album().isEmpty()) song.set_album(album_source_.album());
      if (song.album_id().isEmpty()) song.set_album_id(album_source_.album_id());
      if (song.albumartist().isEmpty()) song.set_albumartist(album_source_.albumartist().isEmpty() ? album_source_.artist() : album_source_.albumartist());
      if (song.artist().isEmpty()) song.set_artist(song.albumartist());
      if (!album_source_.art_automatic().isValid() && !song.art_automatic().isValid()) song.set_art_automatic(album_source_.art_automatic());
    }

    const QString key = song.song_id().isEmpty() ? (song.artist_id().isEmpty() ? song.album_id() : song.artist_id()) : song.song_id();
    if (key.isEmpty() || songs_.contains(key)) continue;
    songs_.insert(key, song);

  }

}

QString YouTubeMusicRequest::ParseTitle(const QJsonObject &item) const {

  QStringList title_runs = TextRuns(item["title"_L1]);
  if (!title_runs.isEmpty()) return title_runs.join(QString());

  const QJsonValue flex_columns = item["flexColumns"_L1];
  if (flex_columns.isArray()) {
    const QJsonArray array = flex_columns.toArray();
    if (!array.isEmpty()) {
      const QJsonObject renderer = array[0].toObject()["musicResponsiveListItemFlexColumnRenderer"_L1].toObject();
      return TextRuns(renderer["text"_L1]).join(QString());
    }
  }

  return QString();

}

Song YouTubeMusicRequest::ParseSongItem(const QJsonObject &item) const {

  const QString title = ParseTitle(item);
  const QString video_id = VideoId(item);
  if (title.isEmpty() || video_id.isEmpty()) return Song();

  QStringList subtitles = Subtitles(item);
  StripItemType(subtitles);

  QString duration_text;
  if (!subtitles.isEmpty() && LooksLikeDuration(subtitles.last())) {
    duration_text = subtitles.takeLast();
  }

  if (duration_text.isEmpty()) {
    // Album track rows carry the length in a fixed column instead.
    const QJsonArray fixed_columns = item["fixedColumns"_L1].toArray();
    if (!fixed_columns.isEmpty()) {
      const QJsonObject renderer = fixed_columns.first().toObject()["musicResponsiveListItemFixedColumnRenderer"_L1].toObject();
      const QString text = TextRuns(renderer["text"_L1]).join(QString());
      if (LooksLikeDuration(text)) duration_text = text;
    }
  }

  const QString artist = subtitles.value(0);
  const QString album = subtitles.value(1);

  Song song(Song::Source::YouTubeMusic);
  song.set_valid(true);
  song.set_directory_id(0);
  song.set_title(title);
  song.set_artist(artist);
  song.set_albumartist(artist);
  song.set_album(album);
  song.set_song_id(video_id);
  song.set_url(QUrl(QStringLiteral("youtubemusic://song/%1").arg(video_id)));
  song.set_length_nanosec(DurationFromText(duration_text));
  song.set_art_automatic(LargestThumbnail(item));

  return song;

}

Song YouTubeMusicRequest::ParseArtistItem(const QJsonObject &item) const {

  QString title = ParseTitle(item);
  if (title.isEmpty()) return Song();

  // The first line of an artist row is the artist, followed by "Artist • 67.8M
  // monthly audience" or the album/artist pair in a mixed response.
  const QString artist_id = BrowseId(item, {u"MUSIC_PAGE_TYPE_ARTIST"_s, u"MUSIC_PAGE_TYPE_LIBRARY_ARTIST"_s});
  if (artist_id.isEmpty()) return Song();

  Song song(Song::Source::YouTubeMusic);
  song.set_valid(true);
  song.set_directory_id(0);
  song.set_title(title);
  song.set_artist(title);
  song.set_artist_id(artist_id);
  song.set_url(QUrl(QStringLiteral("youtubemusic://artist/%1").arg(artist_id)));
  song.set_art_automatic(LargestThumbnail(item));

  return song;

}

Song YouTubeMusicRequest::ParseAlbumItem(const QJsonObject &item, const bool strict_page_type) const {

  const QString title = ParseTitle(item);
  if (title.isEmpty()) return Song();

  const QString album_id = BrowseId(item, {u"MUSIC_PAGE_TYPE_ALBUM"_s}, strict_page_type);
  if (album_id.isEmpty()) return Song();

  QStringList subtitles = Subtitles(item);
  StripItemType(subtitles);

  int year = 0;
  if (!subtitles.isEmpty()) {
    bool ok = false;
    const int parsed = subtitles.last().toInt(&ok);
    if (ok && parsed > 1000 && parsed < 3000) {
      year = parsed;
      subtitles.removeLast();
    }
  }

  const QString album_artist = subtitles.value(0);

  Song song(Song::Source::YouTubeMusic);
  song.set_valid(true);
  song.set_directory_id(0);
  song.set_title(title);
  song.set_album(title);
  song.set_albumartist(album_artist);
  song.set_artist(album_artist);
  song.set_album_id(album_id);
  song.set_year(year);
  song.set_url(QUrl(QStringLiteral("youtubemusic://album/%1").arg(album_id)));
  song.set_art_automatic(LargestThumbnail(item));

  return song;

}

QString YouTubeMusicRequest::FindContinuation(const QJsonObject &json_object) const {

  // Both shapes are in use: musicShelfRenderer.continuations[].nextContinuationData
  // and continuationItemRenderer.continuationEndpoint.continuationCommand.token.
  QList<QJsonObject> command_objects;
  CollectObjectsWithKey(json_object, u"continuationCommand"_s, command_objects);
  for (const QJsonObject &command : command_objects) {
    const QString token = command["token"_L1].toString();
    if (!token.isEmpty()) return token;
  }

  QList<QJsonObject> next_objects;
  CollectObjectsWithKey(json_object, u"nextContinuationData"_s, next_objects);
  for (const QJsonObject &next : next_objects) {
    const QString token = next["continuation"_L1].toString();
    if (!token.isEmpty()) return token;
  }

  return QString();

}

void YouTubeMusicRequest::RequestContinuation(const QString &token) {

  QJsonObject body;
  body["context"_L1] = CreateContext();
  body["continuation"_L1] = token;

  const bool is_search = type_ == Type::SearchArtists || type_ == Type::SearchAlbums || type_ == Type::SearchSongs;
  Send(is_search ? u"/youtubei/v1/search"_s : u"/youtubei/v1/browse"_s, body);

}

void YouTubeMusicRequest::EmitResults() {

  if (songs_.isEmpty() && error_.isEmpty()) {
    error_ = (type_ == Type::LibraryArtists || type_ == Type::LibraryAlbums || type_ == Type::LibrarySongs)
                 ? tr("No songs found in the YouTube Music library. Log in with cookies in the YouTube Music settings to see your library.")
                 : tr("No results.");
  }

  Q_EMIT Results(search_id_, songs_, error_);

}

void YouTubeMusicRequest::Send(const QString &endpoint, const QJsonObject &body) {

  last_endpoint_ = endpoint;
  last_body_ = body;

  reply_ = CreateInnerTubeRequest(endpoint, body);
  QObject::connect(reply_, &QNetworkReply::finished, this, &YouTubeMusicRequest::ReplyFinished);

}
