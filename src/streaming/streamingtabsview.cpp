/*
 * Strawberry Music Player
 * Copyright 2018-2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <QtGlobal>
#include <QWidget>
#include <QVariant>
#include <QString>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTabWidget>
#include <functional>

#include <QTimer>
#include <QTreeView>
#include <QStackedWidget>
#include <QContextMenuEvent>
#include <QAction>
#include <QSettings>

#include "core/iconloader.h"
#include "core/logging.h"
#include "core/songmimedata.h"
#include "core/settings.h"
#include "collection/collectionbackend.h"
#include "collection/collectionmodel.h"
#include "collection/collectionfilter.h"
#include "collection/collectionfilterwidget.h"
#include "streamingservice.h"
#include "streamingtabsview.h"
#include "streamingcollectionview.h"
#include "streamingcollectionviewcontainer.h"
#include "ui_streamingtabsview.h"

using namespace Qt::Literals::StringLiterals;

namespace {
constexpr char kTab[] = "tab";
constexpr char kDefaultTab[] = "artists";
}  // namespace

StreamingTabsView::StreamingTabsView(const StreamingServicePtr service, const SharedPtr<AlbumCoverLoader> albumcover_loader, const QString &settings_group, QWidget *parent)
    : QWidget(parent),
      service_(service),
      settings_group_(settings_group),
      ui_(new Ui_StreamingTabsView) {

  ui_->setupUi(this);

  ui_->search_view->Init(service, albumcover_loader);
  QObject::connect(ui_->search_view, &StreamingSearchView::AddArtistsSignal, &*service_, &StreamingService::AddArtists);
  QObject::connect(ui_->search_view, &StreamingSearchView::AddAlbumsSignal, &*service_, &StreamingService::AddAlbums);
  QObject::connect(ui_->search_view, &StreamingSearchView::AddSongsSignal, &*service_, &StreamingService::AddSongs);

  QAction *action_configure = new QAction(IconLoader::Load(u"configure"_s), tr("Configure %1...").arg(Song::TextForSource(service_->source())), this);
  QObject::connect(action_configure, &QAction::triggered, this, &StreamingTabsView::Configure);

  if (service_->artists_collection_model()) {
    ui_->artists_collection->stacked()->setCurrentWidget(ui_->artists_collection->streamingcollection_page());
    ui_->artists_collection->view()->Init(service_->artists_collection_backend(), service_->artists_collection_model(), true, service_->supports_downloads(), service_->supports_album_tracks());
    QObject::connect(ui_->artists_collection->view(), &StreamingCollectionView::PlaySongsSignal, this, &StreamingTabsView::PlaySongs);
    QObject::connect(ui_->artists_collection->view(), &StreamingCollectionView::QueueSongsSignal, this, &StreamingTabsView::QueueSongs);
    QObject::connect(ui_->artists_collection->view(), &StreamingCollectionView::OpenSongsInNewPlaylistSignal, this, &StreamingTabsView::OpenSongsInNewPlaylist);
    ui_->artists_collection->view()->setModel(service_->artists_collection_filter_model());
    ui_->artists_collection->view()->SetFilter(ui_->artists_collection->filter_widget());
    ui_->artists_collection->filter_widget()->SetSettingsGroup(settings_group);
    ui_->artists_collection->filter_widget()->SetSettingsPrefix(u"artists"_s);
    ui_->artists_collection->filter_widget()->Init(service_->artists_collection_model(), service_->artists_collection_filter_model());
    ui_->artists_collection->filter_widget()->AddMenuAction(action_configure);

    QObject::connect(ui_->artists_collection->view(), &StreamingCollectionView::GetSongs, this, &StreamingTabsView::GetArtists);
    QObject::connect(ui_->artists_collection->view(), &QAbstractItemView::doubleClicked, this, &StreamingTabsView::ArtistDoubleClicked);
    QObject::connect(ui_->artists_collection->view(), &QTreeView::expanded, this, &StreamingTabsView::ArtistsRowExpanded);
    QObject::connect(ui_->artists_collection->view(), &StreamingCollectionView::RemoveSongs, &*service_, &StreamingService::RemoveArtists);

    QObject::connect(ui_->artists_collection->button_refresh(), &QPushButton::clicked, this, &StreamingTabsView::GetArtists);
    QObject::connect(ui_->artists_collection->button_close(), &QPushButton::clicked, this, &StreamingTabsView::AbortGetArtists);
    QObject::connect(ui_->artists_collection->button_abort(), &QPushButton::clicked, this, &StreamingTabsView::AbortGetArtists);
    QObject::connect(&*service_, &StreamingService::ArtistsResults, this, &StreamingTabsView::ArtistsFinished);
    QObject::connect(&*service_, &StreamingService::ArtistsUpdateStatus, ui_->artists_collection->status(), &QLabel::setText);
    QObject::connect(&*service_, &StreamingService::ArtistsProgressSetMaximum, ui_->artists_collection->progressbar(), &QProgressBar::setMaximum);
    QObject::connect(&*service_, &StreamingService::ArtistsUpdateProgress, ui_->artists_collection->progressbar(), &QProgressBar::setValue);

    QObject::connect(service_->artists_collection_model(), &CollectionModel::TotalArtistCountUpdated, ui_->artists_collection->view(), &StreamingCollectionView::TotalArtistCountUpdated);
    QObject::connect(service_->artists_collection_model(), &CollectionModel::TotalAlbumCountUpdated, ui_->artists_collection->view(), &StreamingCollectionView::TotalAlbumCountUpdated);
    QObject::connect(service_->artists_collection_model(), &CollectionModel::TotalSongCountUpdated, ui_->artists_collection->view(), &StreamingCollectionView::TotalSongCountUpdated);
    QObject::connect(service_->artists_collection_model(), &CollectionModel::modelAboutToBeReset, ui_->artists_collection->view(), &StreamingCollectionView::SaveFocus);
    QObject::connect(service_->artists_collection_model(), &CollectionModel::modelReset, ui_->artists_collection->view(), &StreamingCollectionView::RestoreFocus);

  }
  else {
    ui_->tabs->removeTab(ui_->tabs->indexOf(ui_->artists));
  }

  if (service_->albums_collection_model()) {
    ui_->albums_collection->stacked()->setCurrentWidget(ui_->albums_collection->streamingcollection_page());
    ui_->albums_collection->view()->Init(service_->albums_collection_backend(), service_->albums_collection_model(), true, service_->supports_downloads(), service_->supports_album_tracks());
    QObject::connect(ui_->albums_collection->view(), &StreamingCollectionView::PlaySongsSignal, this, &StreamingTabsView::PlaySongs);
    QObject::connect(ui_->albums_collection->view(), &StreamingCollectionView::QueueSongsSignal, this, &StreamingTabsView::QueueSongs);
    QObject::connect(ui_->albums_collection->view(), &StreamingCollectionView::OpenSongsInNewPlaylistSignal, this, &StreamingTabsView::OpenSongsInNewPlaylist);
    ui_->albums_collection->view()->setModel(service_->albums_collection_filter_model());
    ui_->albums_collection->view()->SetFilter(ui_->albums_collection->filter_widget());
    ui_->albums_collection->filter_widget()->SetSettingsGroup(settings_group);
    ui_->albums_collection->filter_widget()->SetSettingsPrefix(u"albums"_s);
    ui_->albums_collection->filter_widget()->Init(service_->albums_collection_model(), service_->albums_collection_filter_model());
    ui_->albums_collection->filter_widget()->AddMenuAction(action_configure);

    QObject::connect(ui_->albums_collection->view(), &StreamingCollectionView::GetSongs, this, &StreamingTabsView::GetAlbums);
    QObject::connect(ui_->albums_collection->view(), &QAbstractItemView::doubleClicked, this, &StreamingTabsView::AlbumDoubleClicked);
    QObject::connect(ui_->albums_collection->view(), &QTreeView::expanded, this, &StreamingTabsView::AlbumsRowExpanded);
    QObject::connect(ui_->albums_collection->view(), &StreamingCollectionView::RemoveSongs, &*service_, &StreamingService::RemoveAlbums);

    QObject::connect(ui_->albums_collection->button_refresh(), &QPushButton::clicked, this, &StreamingTabsView::GetAlbums);
    QObject::connect(ui_->albums_collection->button_close(), &QPushButton::clicked, this, &StreamingTabsView::AbortGetAlbums);
    QObject::connect(ui_->albums_collection->button_abort(), &QPushButton::clicked, this, &StreamingTabsView::AbortGetAlbums);
    QObject::connect(&*service_, &StreamingService::AlbumsResults, this, &StreamingTabsView::AlbumsFinished);
    QObject::connect(&*service_, &StreamingService::AlbumsUpdateStatus, ui_->albums_collection->status(), &QLabel::setText);
    QObject::connect(&*service_, &StreamingService::AlbumsProgressSetMaximum, ui_->albums_collection->progressbar(), &QProgressBar::setMaximum);
    QObject::connect(&*service_, &StreamingService::AlbumsUpdateProgress, ui_->albums_collection->progressbar(), &QProgressBar::setValue);

    QObject::connect(service_->albums_collection_model(), &CollectionModel::TotalArtistCountUpdated, ui_->albums_collection->view(), &StreamingCollectionView::TotalArtistCountUpdated);
    QObject::connect(service_->albums_collection_model(), &CollectionModel::TotalAlbumCountUpdated, ui_->albums_collection->view(), &StreamingCollectionView::TotalAlbumCountUpdated);
    QObject::connect(service_->albums_collection_model(), &CollectionModel::TotalSongCountUpdated, ui_->albums_collection->view(), &StreamingCollectionView::TotalSongCountUpdated);
    QObject::connect(service_->albums_collection_model(), &CollectionModel::modelAboutToBeReset, ui_->albums_collection->view(), &StreamingCollectionView::SaveFocus);
    QObject::connect(service_->albums_collection_model(), &CollectionModel::modelReset, ui_->albums_collection->view(), &StreamingCollectionView::RestoreFocus);

  }
  else {
    ui_->tabs->removeTab(ui_->tabs->indexOf(ui_->albums));
  }

  if (service_->songs_collection_model()) {
    ui_->songs_collection->stacked()->setCurrentWidget(ui_->songs_collection->streamingcollection_page());
    ui_->songs_collection->view()->Init(service_->songs_collection_backend(), service_->songs_collection_model(), true, service_->supports_downloads(), service_->supports_album_tracks());
    QObject::connect(ui_->songs_collection->view(), &StreamingCollectionView::PlaySongsSignal, this, &StreamingTabsView::PlaySongs);
    QObject::connect(ui_->songs_collection->view(), &StreamingCollectionView::QueueSongsSignal, this, &StreamingTabsView::QueueSongs);
    QObject::connect(ui_->songs_collection->view(), &StreamingCollectionView::OpenSongsInNewPlaylistSignal, this, &StreamingTabsView::OpenSongsInNewPlaylist);
    ui_->songs_collection->view()->setModel(service_->songs_collection_filter_model());
    ui_->songs_collection->view()->SetFilter(ui_->songs_collection->filter_widget());
    ui_->songs_collection->filter_widget()->SetSettingsGroup(settings_group);
    ui_->songs_collection->filter_widget()->SetSettingsPrefix(u"songs"_s);
    ui_->songs_collection->filter_widget()->Init(service_->songs_collection_model(), service_->songs_collection_filter_model());
    ui_->songs_collection->filter_widget()->AddMenuAction(action_configure);

    QObject::connect(ui_->songs_collection->view(), &StreamingCollectionView::GetSongs, this, &StreamingTabsView::GetSongs);
    QObject::connect(ui_->songs_collection->view(), &StreamingCollectionView::RemoveSongs, &*service_, &StreamingService::RemoveSongsByList);

    QObject::connect(ui_->songs_collection->button_refresh(), &QPushButton::clicked, this, &StreamingTabsView::GetSongs);
    QObject::connect(ui_->songs_collection->button_close(), &QPushButton::clicked, this, &StreamingTabsView::AbortGetSongs);
    QObject::connect(ui_->songs_collection->button_abort(), &QPushButton::clicked, this, &StreamingTabsView::AbortGetSongs);
    QObject::connect(&*service_, &StreamingService::SongsResults, this, &StreamingTabsView::SongsFinished);
    QObject::connect(&*service_, &StreamingService::SongsUpdateStatus, ui_->songs_collection->status(), &QLabel::setText);
    QObject::connect(&*service_, &StreamingService::SongsProgressSetMaximum, ui_->songs_collection->progressbar(), &QProgressBar::setMaximum);
    QObject::connect(&*service_, &StreamingService::SongsUpdateProgress, ui_->songs_collection->progressbar(), &QProgressBar::setValue);

    tracks_action_ = TracksAction::None;
  QObject::connect(&*service_, &StreamingService::AlbumTracksLoaded, this, &StreamingTabsView::AlbumTracksReceived);

  // Album rows are children of artist rows here, and a cover pixmap would be drawn at
  // kPrettyCoverSize while the artist icon is a 16px icon - use one icon size.
  service_->artists_collection_model()->SetUseSmallIcons(true);
  service_->albums_collection_model()->SetUseSmallIcons(true);
  service_->songs_collection_model()->SetUseSmallIcons(true);
  QObject::connect(service_->songs_collection_model(), &CollectionModel::TotalArtistCountUpdated, ui_->songs_collection->view(), &StreamingCollectionView::TotalArtistCountUpdated);
    QObject::connect(service_->songs_collection_model(), &CollectionModel::TotalAlbumCountUpdated, ui_->songs_collection->view(), &StreamingCollectionView::TotalAlbumCountUpdated);
    QObject::connect(service_->songs_collection_model(), &CollectionModel::TotalSongCountUpdated, ui_->songs_collection->view(), &StreamingCollectionView::TotalSongCountUpdated);
    QObject::connect(service_->songs_collection_model(), &CollectionModel::modelAboutToBeReset, ui_->songs_collection->view(), &StreamingCollectionView::SaveFocus);
    QObject::connect(service_->songs_collection_model(), &CollectionModel::modelReset, ui_->songs_collection->view(), &StreamingCollectionView::RestoreFocus);

  }
  else {
    ui_->tabs->removeTab(ui_->tabs->indexOf(ui_->songs));
  }

  Settings s;
  s.beginGroup(settings_group_);
  const QString tab = s.value(kTab, QLatin1String(kDefaultTab)).toString().toLower();
  s.endGroup();

  if (tab == "artists"_L1) {
    ui_->tabs->setCurrentWidget(ui_->artists);
  }
  else if (tab == "albums"_L1) {
    ui_->tabs->setCurrentWidget(ui_->albums);
  }
  else if (tab == "songs"_L1) {
    ui_->tabs->setCurrentWidget(ui_->songs);
  }
  else if (tab == "search"_L1) {
    ui_->tabs->setCurrentWidget(ui_->search);
  }

  ReloadSettings();

}

StreamingTabsView::~StreamingTabsView() {

  Settings s;
  s.beginGroup(settings_group_);
  if (QWidget *current = ui_->tabs->currentWidget()) {
    s.setValue(kTab, current->objectName().toLower());
  }
  s.endGroup();

  delete ui_;

}

void StreamingTabsView::ReloadSettings() {

  if (service_->artists_collection_model()) {
    ui_->artists_collection->view()->ReloadSettings();
  }
  if (service_->albums_collection_model()) {
    ui_->albums_collection->view()->ReloadSettings();
  }
  if (service_->songs_collection_model()) {
    ui_->songs_collection->view()->ReloadSettings();
  }
  ui_->search_view->ReloadSettings();

}

bool StreamingTabsView::SearchFieldHasFocus() const {

  return ((ui_->tabs->currentWidget() == ui_->artists && ui_->artists_collection->SearchFieldHasFocus()) ||
      (ui_->tabs->currentWidget() == ui_->albums && ui_->albums_collection->SearchFieldHasFocus()) ||
      (ui_->tabs->currentWidget() == ui_->songs && ui_->songs_collection->SearchFieldHasFocus()) ||
      (ui_->tabs->currentWidget() == ui_->search && ui_->search_view->SearchFieldHasFocus()));

}

void StreamingTabsView::FocusSearchField() {

  if (ui_->tabs->currentWidget() == ui_->artists) {
    ui_->artists_collection->FocusSearchField();
  }
  else if (ui_->tabs->currentWidget() == ui_->albums) {
    ui_->albums_collection->FocusSearchField();
  }
  else if (ui_->tabs->currentWidget() == ui_->songs) {
    ui_->songs_collection->FocusSearchField();
  }
  else if (ui_->tabs->currentWidget() == ui_->search) {
    ui_->search_view->FocusSearchField();
  }

}

void StreamingTabsView::GetArtists() {

  if (!service_->authenticated() && service_->oauth()) {
    Configure();
    return;
  }

  ui_->artists_collection->status()->clear();
  ui_->artists_collection->progressbar()->show();
  ui_->artists_collection->button_abort()->show();
  ui_->artists_collection->button_close()->hide();
  ui_->artists_collection->stacked()->setCurrentWidget(ui_->artists_collection->help_page());
  service_->GetArtists();

}

void StreamingTabsView::AbortGetArtists() {

  service_->ResetArtistsRequest();
  ui_->artists_collection->progressbar()->setValue(0);
  ui_->artists_collection->status()->clear();
  ui_->artists_collection->stacked()->setCurrentWidget(ui_->artists_collection->streamingcollection_page());

}

void StreamingTabsView::ArtistsFinished(const SongMap &songs, const QString &error) {

  if (songs.isEmpty() && !error.isEmpty()) {
    ui_->artists_collection->status()->setText(error);
    ui_->artists_collection->progressbar()->setValue(0);
    ui_->artists_collection->progressbar()->hide();
    ui_->artists_collection->button_abort()->hide();
    ui_->artists_collection->button_close()->show();
  }
  else {
    ui_->artists_collection->stacked()->setCurrentWidget(ui_->artists_collection->streamingcollection_page());
    ui_->artists_collection->status()->clear();
    service_->artists_collection_backend()->UpdateSongsBySongIDAsync(songs);
  }

}

void StreamingTabsView::GetAlbums() {

  if (!service_->authenticated() && service_->oauth()) {
    Configure();
    return;
  }

  ui_->albums_collection->status()->clear();
  ui_->albums_collection->progressbar()->show();
  ui_->albums_collection->button_abort()->show();
  ui_->albums_collection->button_close()->hide();
  ui_->albums_collection->stacked()->setCurrentWidget(ui_->albums_collection->help_page());
  service_->GetAlbums();

}

void StreamingTabsView::AlbumDoubleClicked(const QModelIndex &index) {

  Q_UNUSED(index);

  // Double clicking an album/artist row loads its tracks instead of adding the
  // (unplayable) container row itself to the playlist.
  if (!service_->supports_album_tracks()) return;

  const SongList songs = ui_->albums_collection->view()->GetSelectedSongs();
  for (const Song &song : songs) {
    if (song.song_id().isEmpty() && !song.album_id().isEmpty()) {
      tracks_action_ = TracksAction::Playlist;
      service_->GetAlbumTracks(song);
    }
  }

}

void StreamingTabsView::ArtistDoubleClicked(const QModelIndex &index) {

  Q_UNUSED(index);

  if (!service_->supports_album_tracks()) return;

  const SongList songs = ui_->artists_collection->view()->GetSelectedSongs();
  for (const Song &song : songs) {
    if (song.song_id().isEmpty() && !song.artist_id().isEmpty()) {
      tracks_action_ = TracksAction::Playlist;
      service_->GetArtistTracks(song);
    }
  }

}

void StreamingTabsView::ArtistsRowExpanded(const QModelIndex &index) {

  ExpandContainerRow(ui_->artists_collection->view(), index, true);

}

void StreamingTabsView::AlbumsRowExpanded(const QModelIndex &index) {

  ExpandContainerRow(ui_->albums_collection->view(), index, false);

}





void StreamingTabsView::ExpandContainerRow(QTreeView *view, const QModelIndex &index, const bool artists_view) {

  if (!service_->supports_album_tracks()) return;

  // Rows live in the collection, so a container that already lists children was expanded
  // before (possibly in an earlier session) - nothing to fetch.
  if (!index.isValid() || view->model()->rowCount(index) > 0) return;

  const QString container_id = index.data(CollectionModel::Role_ContainerId).toString();
  // DisplayRole is the human name; Role_ContainerKey is a composite key
  // ("artist-album-id") and must never be used as an album or artist name.
  const QString container_name = index.data(Qt::DisplayRole).toString();
  if (container_id.isEmpty()) return;

  const int group_by = index.data(CollectionModel::Role_ContainerType).toInt();
  const bool is_artist_level = group_by == static_cast<int>(CollectionModel::GroupBy::Artist) ||
                               group_by == static_cast<int>(CollectionModel::GroupBy::AlbumArtist);

  if (is_artist_level) {
    Song artist(Song::Source::YouTubeMusic);
    artist.set_url(QUrl(QStringLiteral("youtubemusic://artist/%1").arg(container_id)));
    artist.set_artist(container_name);
    artist.set_artist_id(container_id);
    service_->ExpandArtistRow(artist);
    return;
  }

  // Album level: keep the parent artist name so the tracks group under that artist.
  Song album(Song::Source::YouTubeMusic);
  album.set_url(QUrl(QStringLiteral("youtubemusic://album/%1").arg(container_id)));
  album.set_album(container_name);
  album.set_album_id(container_id);
  album.set_artist(index.parent().data().toString());
  album.set_albumartist(index.parent().data().toString());
  service_->ExpandAlbumRow(album, artists_view);

}

void StreamingTabsView::PlaySongs(const SongList &songs, const bool replace, const bool play) {

  if (!replace) ResolveOrEmitSongs(songs, TracksAction::Add);
  else ResolveOrEmitSongs(songs, play ? TracksAction::Play : TracksAction::Replace);

}

void StreamingTabsView::OpenSongsInNewPlaylist(const SongList &songs) {

  ResolveOrEmitSongs(songs, TracksAction::NewPlaylist);

}



void StreamingTabsView::QueueSongs(const SongList &songs, const bool next) {

  ResolveOrEmitSongs(songs, next ? TracksAction::EnqueueNext : TracksAction::Enqueue);

}

void StreamingTabsView::ResolveOrEmitSongs(const SongList &songs, const TracksAction action) {

  SongList tracks;

  for (Song song : songs) {
    // A song without a playable url must never reach the playlist: the engine answers
    // with 'Invalid URI ""' and the entry cannot be played at all. Songs that only carry
    // an id (search results built from an id, playlist rows restored from a file) get
    // their url reconstructed.
    if (!song.url().isValid() || song.url().isEmpty()) {
      // Container rows (album/artist) are resolved separately, never reconstructed here.
      if (!song.album_id().isEmpty() || !song.artist_id().isEmpty()) continue;
      if (song.song_id().isEmpty()) {
        qLog(Warning) << "Skipping song without url or id:" << song.title();
        continue;
      }
      song.set_url(QUrl(QStringLiteral("youtubemusic://song/%1").arg(song.song_id())));
    }
    if (song.song_id().isEmpty() && (!song.album_id().isEmpty() || !song.artist_id().isEmpty())) {
      // Container row: the tracks arrive asynchronously.
      if (!service_->supports_album_tracks()) continue;
      tracks_action_ = action;
      if (!song.album_id().isEmpty()) service_->GetAlbumTracks(song);
      else service_->GetArtistTracks(song);
    }
    else {
      tracks << song;
    }
  }

  if (tracks.isEmpty()) return;

  SongMimeData *mimedata = new SongMimeData;
  mimedata->songs = tracks;
  mimedata->override_user_settings_ = true;

  switch (action) {
    case TracksAction::Play:
      // override_user_settings_ matters: without it MainWindow re-applies the user's
      // menu behaviour on top of these flags and the action turns into an append.
      mimedata->override_user_settings_ = true;
      mimedata->clear_first_ = true;
      mimedata->play_now_ = true;
      break;
    case TracksAction::Replace:
      mimedata->override_user_settings_ = true;
      mimedata->clear_first_ = true;
      break;
    case TracksAction::NewPlaylist:
      mimedata->open_in_new_playlist_ = true;
      break;
    case TracksAction::Enqueue:
      mimedata->override_user_settings_ = true;
      mimedata->enqueue_now_ = true;
      break;
    case TracksAction::EnqueueNext:
      mimedata->override_user_settings_ = true;
      mimedata->enqueue_next_now_ = true;
      break;
    case TracksAction::Add:
    case TracksAction::Playlist:
    case TracksAction::None:
      break;
  }

  Q_EMIT ui_->albums_collection->view()->AddToPlaylistSignal(mimedata);

}

void StreamingTabsView::AlbumTracksReceived(const SongList &songs, const QString &error) {

  if (songs.isEmpty()) {
    if (!error.isEmpty()) Q_EMIT OpenSettingsDialog(service_->source());
    return;
  }

  SongMimeData *mimedata = new SongMimeData;
  mimedata->songs = songs;

  switch (tracks_action_) {
    case TracksAction::Play:
      mimedata->override_user_settings_ = true;
      mimedata->clear_first_ = true;
      mimedata->play_now_ = true;
      break;
    case TracksAction::Replace:
      mimedata->override_user_settings_ = true;
      mimedata->clear_first_ = true;
      break;
    case TracksAction::Add:
      mimedata->override_user_settings_ = true;
      break;
    case TracksAction::NewPlaylist:
      mimedata->override_user_settings_ = true;
      mimedata->open_in_new_playlist_ = true;
      break;
    case TracksAction::Enqueue:
      mimedata->override_user_settings_ = true;
      mimedata->enqueue_now_ = true;
      break;
    case TracksAction::EnqueueNext:
      mimedata->override_user_settings_ = true;
      mimedata->enqueue_next_now_ = true;
      break;
    case TracksAction::Playlist:
      mimedata->from_doubleclick_ = true;
      break;
    case TracksAction::None:
      break;
  }
  tracks_action_ = TracksAction::None;

  // MainWindow::AddToPlaylist() takes ownership and deletes the mime data, so
  // nothing may touch it afterwards.
  Q_EMIT ui_->albums_collection->view()->AddToPlaylistSignal(mimedata);

}

void StreamingTabsView::AbortGetAlbums() {

  service_->ResetAlbumsRequest();
  ui_->albums_collection->progressbar()->setValue(0);
  ui_->albums_collection->status()->clear();
  ui_->albums_collection->stacked()->setCurrentWidget(ui_->albums_collection->streamingcollection_page());

}

void StreamingTabsView::AlbumsFinished(const SongMap &songs, const QString &error) {

  if (songs.isEmpty() && !error.isEmpty()) {
    ui_->albums_collection->status()->setText(error);
    ui_->albums_collection->progressbar()->setValue(0);
    ui_->albums_collection->progressbar()->hide();
    ui_->albums_collection->button_abort()->hide();
    ui_->albums_collection->button_close()->show();
  }
  else {
    ui_->albums_collection->stacked()->setCurrentWidget(ui_->albums_collection->streamingcollection_page());
    ui_->albums_collection->status()->clear();
    service_->albums_collection_backend()->UpdateSongsBySongIDAsync(songs);
  }

}

void StreamingTabsView::GetSongs() {

  if (!service_->authenticated() && service_->oauth()) {
    Configure();
    return;
  }

  ui_->songs_collection->status()->clear();
  ui_->songs_collection->progressbar()->show();
  ui_->songs_collection->button_abort()->show();
  ui_->songs_collection->button_close()->hide();
  ui_->songs_collection->stacked()->setCurrentWidget(ui_->songs_collection->help_page());
  service_->GetSongs();

}

void StreamingTabsView::AbortGetSongs() {

  service_->ResetSongsRequest();
  ui_->songs_collection->progressbar()->setValue(0);
  ui_->songs_collection->status()->clear();
  ui_->songs_collection->stacked()->setCurrentWidget(ui_->songs_collection->streamingcollection_page());

}

void StreamingTabsView::SongsFinished(const SongMap &songs, const QString &error) {

  if (songs.isEmpty() && !error.isEmpty()) {
    ui_->songs_collection->status()->setText(error);
    ui_->songs_collection->progressbar()->setValue(0);
    ui_->songs_collection->progressbar()->hide();
    ui_->songs_collection->button_abort()->hide();
    ui_->songs_collection->button_close()->show();
  }
  else {
    ui_->songs_collection->stacked()->setCurrentWidget(ui_->songs_collection->streamingcollection_page());
    ui_->songs_collection->status()->clear();
    service_->songs_collection_backend()->UpdateSongsBySongIDAsync(songs);
  }

}

void StreamingTabsView::Configure() {
  Q_EMIT OpenSettingsDialog(service_->source());
}
