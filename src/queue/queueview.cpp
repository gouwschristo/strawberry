/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2019-2021, Jonas Kvinge <jonas@jkvinge.net>
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

#include <algorithm>
#include <utility>

#include <QWidget>
#include <QAbstractItemModel>
#include <QItemSelectionModel>
#include <QList>
#include <QMetaObject>
#include <QSettings>
#include <QKeySequence>
#include <QLabel>
#include <QToolButton>

#include "includes/shared_ptr.h"
#include "core/iconloader.h"
#include "core/settings.h"
#include "playlist/playlist.h"
#include "playlist/playlistitem.h"
#include <QAction>
#include <QMenu>

#include "playlist/playlistdelegates.h"
#include "playlist/playlistmanager.h"
#include "queue.h"
#include "queueview.h"
#include "ui_queueview.h"
#include "constants/appearancesettings.h"

using namespace Qt::Literals::StringLiterals;

QueueView::QueueView(QWidget *parent)
    : QWidget(parent),
      ui_(new Ui_QueueView),
      playlist_manager_(nullptr),
      current_playlist_(nullptr) {

  ui_->setupUi(this);
  ui_->list->setItemDelegate(new QueuedItemDelegate(this, 0));
  ui_->list->setContextMenuPolicy(Qt::CustomContextMenu);
  QObject::connect(ui_->list, &QWidget::customContextMenuRequested, this, &QueueView::ShowContextMenu);
  // activated() covers both double click and Enter/Return.
  QObject::connect(ui_->list, &QAbstractItemView::activated, this, &QueueView::PlayQueuedItem);

  // Set icons on buttons
  ui_->move_down->setIcon(IconLoader::Load(u"go-down"_s));
  ui_->move_up->setIcon(IconLoader::Load(u"go-up"_s));
  ui_->remove->setIcon(IconLoader::Load(u"edit-delete"_s));
  ui_->clear->setIcon(IconLoader::Load(u"edit-clear-list"_s));

  // Set a standard shortcut
  ui_->remove->setShortcut(QKeySequence::Delete);

  // Button connections
  QObject::connect(ui_->move_down, &QToolButton::clicked, this, &QueueView::MoveDown);
  QObject::connect(ui_->move_up, &QToolButton::clicked, this, &QueueView::MoveUp);
  QObject::connect(ui_->remove, &QToolButton::clicked, this, &QueueView::Remove);
  QObject::connect(ui_->clear, &QToolButton::clicked, this, &QueueView::Clear);

  ReloadSettings();

}

QueueView::~QueueView() {
  delete ui_;
}

QTreeView *QueueView::view() const {

  return ui_->list;

}

void QueueView::PlayQueuedItem(const QModelIndex &index) {

  Playlist *playlist = current_playlist_;
  if (!playlist && playlist_manager_) playlist = playlist_manager_->current();
  if (!playlist || !index.isValid()) return;

  const QModelIndex playlist_index = playlist->queue()->mapToSource(index);
  if (!playlist_index.isValid()) return;

  Q_EMIT PlayRow(playlist_index.row());

}

SongList QueueView::SelectedSongs() const {

  SongList songs;

  // current_playlist_ is set by CurrentPlaylistChanged(), but do not depend on it
  // being set yet when the user is quick with the context menu.
  Playlist *playlist = current_playlist_;
  if (!playlist && playlist_manager_) playlist = playlist_manager_->current();
  if (!playlist) return songs;

  const QModelIndexList selected_rows = ui_->list->selectionModel()->selectedRows();
  for (const QModelIndex &queue_index : selected_rows) {
    const QModelIndex playlist_index = playlist->queue()->mapToSource(queue_index);
    if (!playlist_index.isValid()) continue;
    PlaylistItemPtr item = playlist->item_at(playlist_index.row());
    if (!item) continue;
    const Song song = item->EffectiveMetadata();
    // Playlist items restored from a playlist file often carry only the url, so do not
    // require song_id here - the service resolves the id from the url when needed.
    if (!song.is_stream_service() || !song.url().isValid()) continue;
    songs << song;
  }

  return songs;

}

void QueueView::ShowContextMenu(const QPoint &pos) {

  const SongList songs = SelectedSongs();
  if (songs.isEmpty()) return;

  QMenu menu(this);
  QAction *download = menu.addAction(IconLoader::Load(u"document-save"_s), tr("Download"));
  if (menu.exec(ui_->list->viewport()->mapToGlobal(pos)) == download) {
    Q_EMIT DownloadSongs(songs);
  }

}

void QueueView::SetPlaylistManager(SharedPtr<PlaylistManager> playlist_manager) {

  playlist_manager_ = playlist_manager;

  QObject::connect(&*playlist_manager, &PlaylistManager::CurrentChanged, this, &QueueView::CurrentPlaylistChanged);
  CurrentPlaylistChanged(playlist_manager_->current());

}

void QueueView::ReloadSettings() {

  Settings s;
  s.beginGroup(AppearanceSettings::kSettingsGroup);
  int iconsize = s.value(AppearanceSettings::kIconSizeLeftPanelButtons, AppearanceSettings::kDefaultIconSizeLeftPanelButtons).toInt();
  s.endGroup();

  ui_->move_down->setIconSize(QSize(iconsize, iconsize));
  ui_->move_up->setIconSize(QSize(iconsize, iconsize));
  ui_->remove->setIconSize(QSize(iconsize, iconsize));
  ui_->clear->setIconSize(QSize(iconsize, iconsize));

}

void QueueView::CurrentPlaylistChanged(Playlist *playlist) {

  if (current_playlist_) {
    QObject::disconnect(current_playlist_->queue(), &Queue::rowsInserted, this, &QueueView::UpdateButtonState);
    QObject::disconnect(current_playlist_->queue(), &Queue::rowsRemoved, this, &QueueView::UpdateButtonState);
    QObject::disconnect(current_playlist_->queue(), &Queue::layoutChanged, this, &QueueView::UpdateButtonState);
    QObject::disconnect(current_playlist_->queue(), &Queue::SummaryTextChanged, ui_->summary, &QLabel::setText);
    QObject::disconnect(current_playlist_, &Playlist::destroyed, this, &QueueView::PlaylistDestroyed);
  }

  if (!playlist) {
    current_playlist_ = nullptr;
    return;
  }

  current_playlist_ = playlist;

  QObject::connect(current_playlist_->queue(), &Queue::rowsInserted, this, &QueueView::UpdateButtonState);
  QObject::connect(current_playlist_->queue(), &Queue::rowsRemoved, this, &QueueView::UpdateButtonState);
  QObject::connect(current_playlist_->queue(), &Queue::layoutChanged, this, &QueueView::UpdateButtonState);
  QObject::connect(current_playlist_->queue(), &Queue::SummaryTextChanged, ui_->summary, &QLabel::setText);
  QObject::connect(current_playlist_, &Playlist::destroyed, this, &QueueView::PlaylistDestroyed);

  ui_->list->setModel(current_playlist_->queue());

  QObject::connect(ui_->list->selectionModel(), &QItemSelectionModel::currentChanged, this, &QueueView::UpdateButtonState);
  QObject::connect(ui_->list->selectionModel(), &QItemSelectionModel::selectionChanged, this, &QueueView::UpdateButtonState);

  QMetaObject::invokeMethod(current_playlist_->queue(), &Queue::UpdateSummaryText);

}

void QueueView::MoveUp() {

  QModelIndexList indexes = ui_->list->selectionModel()->selectedRows();
  std::stable_sort(indexes.begin(), indexes.end());

  if (indexes.isEmpty() || indexes.first().row() == 0) return;

  for (const QModelIndex &idx : std::as_const(indexes)) {
    current_playlist_->queue()->MoveUp(idx.row());
  }

}

void QueueView::MoveDown() {

  QModelIndexList indexes = ui_->list->selectionModel()->selectedRows();
  std::stable_sort(indexes.begin(), indexes.end());

  if (indexes.isEmpty() || indexes.last().row() == current_playlist_->queue()->rowCount() - 1) {
    return;
  }

  for (int i = static_cast<int>(indexes.count() - 1); i >= 0; --i) {
    current_playlist_->queue()->MoveDown(indexes[i].row());
  }

}

void QueueView::Clear() {
  current_playlist_->queue()->Clear();
}

void QueueView::Remove() {

  // collect the rows to be removed
  QList<int> row_list;
  const QModelIndexList indexes = ui_->list->selectionModel()->selectedRows();
  for (const QModelIndex &idx : indexes) {
    if (idx.isValid()) row_list << idx.row();
  }

  current_playlist_->queue()->Remove(row_list);

}

void QueueView::UpdateButtonState() {

  if (ui_->list->selectionModel()->selectedRows().count() > 0) {
    ui_->remove->setEnabled(true);
    QModelIndex index_top = ui_->list->model()->index(0, 0);
    QModelIndex index_bottom = ui_->list->model()->index(ui_->list->model()->rowCount() - 1, 0);
    const QModelIndexList selected = ui_->list->selectionModel()->selectedIndexes();
    bool all_selected = ui_->list->selectionModel()->selectedRows().count() == ui_->list->model()->rowCount();
    ui_->move_up->setEnabled(!all_selected && !selected.contains(index_top));
    ui_->move_down->setEnabled(!all_selected && !selected.contains(index_bottom));
  }
  else {
    ui_->move_up->setEnabled(false);
    ui_->move_down->setEnabled(false);
    ui_->remove->setEnabled(false);
  }

  ui_->clear->setEnabled(!current_playlist_->queue()->is_empty());

}

void QueueView::PlaylistDestroyed() {
  current_playlist_ = nullptr;
  // We'll get another CurrentPlaylistChanged() soon
}
