/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICSERVICE_H
#define YOUTUBEMUSICSERVICE_H

#include "config.h"

#include <QList>
#include <QSet>
#include <QMap>
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QScopedPointer>
#include <QSharedPointer>

#include "includes/shared_ptr.h"
#include "core/song.h"
#include "streaming/streamingservice.h"
#include "constants/youtubemusicsettings.h"
#include "collection/collectionmodel.h"

class QTimer;

class TaskManager;
class Database;
class UrlHandlers;
class NetworkAccessManager;
class AlbumCoverLoader;
class YouTubeMusicUrlHandler;
class YouTubeMusicRequest;
class YouTubeMusicWalk;
class YouTubeMusicStreamURLRequest;
class YouTubeMusicDownloadRequest;
class CollectionBackend;
class CollectionModel;
class CollectionFilter;

class YouTubeMusicService;

using YouTubeMusicServicePtr = SharedPtr<YouTubeMusicService>;
using YouTubeMusicRequestPtr = QScopedPointer<YouTubeMusicRequest, QScopedPointerDeleteLater>;
using YouTubeMusicStreamURLRequestPtr = QSharedPointer<YouTubeMusicStreamURLRequest>;

class YouTubeMusicService : public StreamingService {
  Q_OBJECT

 public:
  // One entry per requested download, shown by the download manager dialog.
  struct DownloadEntry {
    enum class State { Queued, Downloading, Done, Failed, Cancelled };
    Song song;
    State state = State::Queued;
    int progress = 0;
    QString path;
    QString error;
  };
  explicit YouTubeMusicService(const SharedPtr<TaskManager> task_manager,
                               const SharedPtr<Database> database,
                               const SharedPtr<NetworkAccessManager> network,
                               const SharedPtr<UrlHandlers> url_handlers,
                               const SharedPtr<AlbumCoverLoader> albumcover_loader,
                               QObject *parent = nullptr);

  ~YouTubeMusicService() override;

  static const Song::Source kSource;

  void Exit() override;
  void ReloadSettings() override;

  bool authenticated() const override;
  bool supports_album_tracks() const override { return true; }
  bool supports_downloads() const override { return true; }
  bool enabled() const { return enabled_; }
  QString cookie() const { return cookie_; }
  QString cookies_browser() const { return cookies_browser_; }
  // Writes the stored cookie header to a Netscape-format cookies file and returns its
  // path (empty when there is no cookie). yt-dlp needs it to get past "sign in to confirm
  // you're not a bot". The file must outlive the yt-dlp process.
  QString WriteCookieFile(QTemporaryFile *file) const;
  QString ytdlp_path() const { return ytdlp_path_; }
  int search_limit() const { return search_limit_; }
  bool remove_remastered() const { return remove_remastered_; }

  QByteArray authorization_header() const;
  QString download_dir() const { return download_dir_; }
  // What the settings page shows instead of the raw cookie: whether there is a session,
  // what is in the library and where the session came from.
  struct SessionInfo {
    bool signed_in = false;
    int songs = 0;
    int artists = 0;
    int albums = 0;
    QString imported_at;
    QString browser;
  };
  SessionInfo session_info() const;

  const QList<DownloadEntry> &download_entries() const { return download_entries_; }
  void CancelCurrentDownload();
  // Collapses artist names that differ only by case, so one artist is not split into
  // two containers in the browser.
  void NormalizeArtistNames();
  // Folds a collaboration entity ("Rezz & PVRIS") into a subscribed artist when one of its
  // parts is a subscribed artist, so the tree only shows artists the user follows.
  void MergeCollaborationArtists();
  void RetryFailedDownloads();
  QString download_format() const { return download_format_; }
  bool download_embed_thumbnail() const { return download_embed_thumbnail_; }

  // Re-reads the YouTube Music cookies from the browser (via the helper script) and
  // loads them into the running session. Returns true when the cookie changed.
  bool RefreshCookies();

  // Reads the session cookies out of a browser with yt-dlp's importer (brave, chrome,
  // chromium, edge, firefox, opera, safari, vivaldi, whale - optionally +KEYRING and
  // :PROFILE) and stores them. Returns true when the stored cookie changed.
  bool ImportCookiesFromBrowser(const QString &browser, QString *error = nullptr);
  // Forgets the stored session.
  void SignOut();
  // Re-imports the browser cookies shortly after start so the first request does not stall.
  void RefreshCookiesInBackground();


  uint GetStreamURL(const QUrl &url, QString &error);

  int Search(const QString &text, const SearchType type) override;
  void CancelSearch() override;

  SharedPtr<CollectionBackend> artists_collection_backend() override { return artists_collection_backend_; }
  SharedPtr<CollectionBackend> albums_collection_backend() override { return albums_collection_backend_; }
  SharedPtr<CollectionBackend> songs_collection_backend() override { return songs_collection_backend_; }

  CollectionModel *artists_collection_model() override { return artists_collection_model_; }
  CollectionModel *albums_collection_model() override { return albums_collection_model_; }
  CollectionModel *songs_collection_model() override { return songs_collection_model_; }

  CollectionFilter *artists_collection_filter_model() override { return artists_collection_model_->filter(); }
  CollectionFilter *albums_collection_filter_model() override { return albums_collection_model_->filter(); }
  CollectionFilter *songs_collection_filter_model() override { return songs_collection_model_->filter(); }

 Q_SIGNALS:
  // The download list (or one entry) changed; the manager dialog refreshes on this.
  void DownloadQueueChanged();
  // Metadata resolved while starting a stream; MainWindow fills in playlist rows that
  // were created from a bare url with it.
  void StreamMetadataResolved(const Song &song);

 public Q_SLOTS:
  void StartSearch();
  void GetArtists() override;
  void GetAlbums() override;
  void GetSongs() override;
  // Asks for more of a list: raises the page budget and refetches it (rows are de-duplicated
  // by id, so this appends to what is already there).
  void LoadMoreArtists() override;
  void LoadMoreAlbums() override;
  void LoadMoreSongs() override;
  void LoadMoreSearch(const QString &query);
  // Walks every library artist whose index entry is missing or stale and stores its albums
  // and tracks, so expanding an artist later is a database read instead of a network walk.
  void IndexLibrary(const int max_artists = -1);
  void CancelIndexLibrary();
  // One stale artist, most recently browsed first. Called from a timer while idle.
  void PrefetchNextArtist();
  bool index_running() const { return index_task_id_ >= 0; }
  void GetAlbumTracks(const Song &album) override;
  void GetArtistTracks(const Song &artist) override;
  void ExpandAlbumRow(const Song &album, const bool artists_view) override;
  void ExpandArtistRow(const Song &artist) override;
  Song ContainerSong(const QString &id, const QString &name, const QString &parent_name, const bool artist_level) const override;
  void GetAllArtistTracks(const Song &artist) override;
  void DownloadSongs(const SongList &songs) override;
  void ResetArtistsRequest() override;
  void ResetAlbumsRequest() override;
  void ResetSongsRequest() override;

 private Q_SLOTS:
  void ExitReceived();
  void ArtistsResultsReceived(const int id, const SongMap &songs, const QString &error);
  void AlbumsResultsReceived(const int id, const SongMap &songs, const QString &error);
  void SongsResultsReceived(const int id, const SongMap &songs, const QString &error);
  void AlbumTracksResultsReceived(const int id, const SongMap &songs, const QString &error);
  void ExpandedAlbumResultsReceived(const int id, const SongMap &songs, const QString &error);
  void ExpandedArtistResultsReceived(const int id, const SongMap &songs, const QString &error);
  void ArtistsUpdateStatusReceived(const QString &text);
  void AlbumsUpdateStatusReceived(const QString &text);
  void SongsUpdateStatusReceived(const QString &text);
  void DownloadFinished(const Song &song, const QString &path, const QString &error);
  void DownloadProgress(const int percent);
  void DownloadContainerTracksReceived(const int id, const SongMap &songs, const QString &error);
  void SearchResultsReceived(const int id, const SongMap &songs, const QString &error);
  void SearchUpdateStatusReceived(const QString &text);
  void HandleStreamURLFailure(const uint id, const QUrl &media_url, const QString &error);
  void HandleStreamMetadata(const QUrl &media_url, const Song &song);
  void HandleStreamURLSuccess(const uint id, const QUrl &media_url, const QUrl &stream_url, const Song::FileType filetype, const int samplerate, const int bit_depth, const qint64 duration);

 private:
  void SendSearch();
  void StartNextDownload();
  void StartNextDownloadContainer();
  void StartNextExpansion();
  void ArtistAlbumsReceived(const int id, const SongMap &songs, const QString &error);
  void StartArtistTracksWalk();
  void FinishArtistTrackWalk();

  // indexer state machine
  void StartNextIndexArtist();
  void IndexArtistAlbumsReceived(const int id, const SongMap &songs, const QString &error);
  void StartIndexAlbumsWalk();
  void FinishIndexAlbum();
  void FinishIndexLibrary(const QString &error = QString());
  void BeginIndex(const QStringList &artist_ids, const QStringList &artist_names);
  void RememberBrowsedArtist(const Song &artist);

  const SharedPtr<TaskManager> task_manager_;
  const SharedPtr<NetworkAccessManager> network_;
  YouTubeMusicUrlHandler *url_handler_;

  SharedPtr<CollectionBackend> artists_collection_backend_;
  SharedPtr<CollectionBackend> albums_collection_backend_;
  SharedPtr<CollectionBackend> songs_collection_backend_;
  CollectionModel *artists_collection_model_;
  CollectionModel *albums_collection_model_;
  CollectionModel *songs_collection_model_;

  QTimer *timer_search_delay_;

  YouTubeMusicRequestPtr artists_request_;
  YouTubeMusicRequestPtr albums_request_;
  YouTubeMusicRequestPtr songs_request_;
  int page_budget_ = 0;   // raised by "load more", 0 = request defaults
  YouTubeMusicRequestPtr search_request_;
  YouTubeMusicRequestPtr album_tracks_request_;
  YouTubeMusicRequestPtr artist_tracks_request_;
  YouTubeMusicRequestPtr expand_album_request_;
  YouTubeMusicRequestPtr expand_artist_request_;

  QMap<uint, YouTubeMusicStreamURLRequestPtr> stream_url_requests_;

  bool enabled_;
  QString cookie_;
  QString ytdlp_path_;
  int search_limit_;
  bool remove_remastered_;
  QString download_dir_;
  QString download_format_;
  bool download_embed_thumbnail_;
  bool auto_refresh_cookies_;
  QString cookies_script_;
  QString cookies_browser_;

  int pending_search_id_;
  QString pending_search_text_;
  SearchType pending_search_type_;
  int next_pending_search_id_;

  int search_id_;
  QString search_text_;

  uint next_stream_url_request_id_;

  QList<QObject*> wait_for_exit_;
  bool expand_into_artists_view_;
  QSet<QString> expanded_albums_;
  QSet<QString> expanded_artists_;
  // Expansions are serialized: overwriting a running request dropped results and left
  // its task stuck in the status bar.
  struct Expansion {
    Song song;
    bool artists_view = false;
    bool is_artist = false;
  };
  QList<Expansion> expand_pending_;

  SharedPtr<Database> database_;
  // Container currently being expanded, used to keep child names consistent.
  Song expand_artist_context_;
  Song expand_album_context_;
  int expand_album_task_id_;
  int expand_artist_task_id_;
  int track_fetch_task_id_;
  QList<DownloadEntry> download_entries_;
  // True while the queued container is an artist: its result is a list of albums to expand.
  bool download_container_is_artist_ = false;
  int current_download_index_;
  SongList download_containers_;
  YouTubeMusicRequestPtr download_container_request_;
  // Walking an artist's albums to collect every track for "add to playlist".
  SongList artist_tracks_albums_;
  SongList artist_tracks_collected_;
  YouTubeMusicRequestPtr artist_walk_request_;
  // One album per request; a few run at a time and the results are kept per album so the
  // collected order stays the album order.
  // indexer
  QStringList index_artist_ids_;
  QStringList index_artist_names_;
  int index_cursor_ = 0;
  SongList index_albums_;
  int index_albums_total_ = 0;
  YouTubeMusicWalk *index_walk_ = nullptr;
  YouTubeMusicRequestPtr index_artist_request_;   // artist -> album list, one per artist
  int index_task_id_ = -1;
  bool index_cancelled_ = false;
  bool index_artist_had_errors_ = false;
  bool prefetch_ = true;
  QTimer *prefetch_timer_ = nullptr;

  QVector<SongList> artist_walk_results_;
  YouTubeMusicWalk *artist_walk_ = nullptr;
  YouTubeMusicDownloadRequest *download_request_;
  int download_task_id_;
};

#endif  // YOUTUBEMUSICSERVICE_H
