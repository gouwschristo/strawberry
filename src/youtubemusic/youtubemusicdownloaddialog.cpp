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

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "core/logging.h"
#include "youtubemusic/youtubemusicservice.h"
#include "youtubemusicdownloaddialog.h"

using namespace Qt::Literals::StringLiterals;

namespace {

QString StateText(const YouTubeMusicService::DownloadEntry &entry) {

  switch (entry.state) {
    case YouTubeMusicService::DownloadEntry::State::Queued:
      return YouTubeMusicDownloadDialog::tr("Queued");
    case YouTubeMusicService::DownloadEntry::State::Downloading:
      return YouTubeMusicDownloadDialog::tr("Downloading");
    case YouTubeMusicService::DownloadEntry::State::Done:
      return YouTubeMusicDownloadDialog::tr("Done");
    case YouTubeMusicService::DownloadEntry::State::Cancelled:
      return YouTubeMusicDownloadDialog::tr("Cancelled");
    case YouTubeMusicService::DownloadEntry::State::Failed:
      return entry.error.isEmpty() ? YouTubeMusicDownloadDialog::tr("Failed") : entry.error;
  }

  return QString();

}

}  // namespace

YouTubeMusicDownloadDialog::YouTubeMusicDownloadDialog(SharedPtr<YouTubeMusicService> service, QWidget *parent)
    : QDialog(parent),
      service_(service),
      table_(new QTableWidget(this)),
      button_cancel_(new QPushButton(tr("Cancel current"), this)),
      button_retry_(new QPushButton(tr("Retry failed"), this)) {

  setWindowTitle(tr("YouTube Music downloads"));
  resize(760, 420);

  table_->setColumnCount(4);
  table_->setHorizontalHeaderLabels({tr("Title"), tr("Artist"), tr("Progress"), tr("Status")});
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
  table_->verticalHeader()->setVisible(false);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);

  QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  buttons->addButton(button_cancel_, QDialogButtonBox::ActionRole);
  buttons->addButton(button_retry_, QDialogButtonBox::ActionRole);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addWidget(table_);
  layout->addWidget(buttons);

  QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
  QObject::connect(button_cancel_, &QPushButton::clicked, this, &YouTubeMusicDownloadDialog::CancelClicked);
  QObject::connect(button_retry_, &QPushButton::clicked, this, &YouTubeMusicDownloadDialog::RetryClicked);

  if (service_) {
    QObject::connect(&*service_, &YouTubeMusicService::DownloadQueueChanged, this, &YouTubeMusicDownloadDialog::Refresh);
  }

  Refresh();

}

YouTubeMusicDownloadDialog::~YouTubeMusicDownloadDialog() {}

void YouTubeMusicDownloadDialog::Refresh() {

  if (!service_) return;

  const QList<YouTubeMusicService::DownloadEntry> &entries = service_->download_entries();

  table_->setRowCount(entries.count());

  bool has_running = false;
  bool has_failed = false;

  for (int row = 0; row < entries.count(); ++row) {
    const YouTubeMusicService::DownloadEntry &entry = entries[row];

    if (entry.state == YouTubeMusicService::DownloadEntry::State::Downloading) has_running = true;
    if (entry.state == YouTubeMusicService::DownloadEntry::State::Failed || entry.state == YouTubeMusicService::DownloadEntry::State::Cancelled) has_failed = true;

    table_->setItem(row, 0, new QTableWidgetItem(entry.song.title()));
    table_->setItem(row, 1, new QTableWidgetItem(entry.song.artist()));

    const QString progress = entry.state == YouTubeMusicService::DownloadEntry::State::Done ? u"100%"_s
                             : entry.state == YouTubeMusicService::DownloadEntry::State::Queued ? QString()
                                                                                               : QStringLiteral("%1%").arg(entry.progress);
    table_->setItem(row, 2, new QTableWidgetItem(progress));

    QTableWidgetItem *status = new QTableWidgetItem(StateText(entry));
    if (entry.state == YouTubeMusicService::DownloadEntry::State::Failed) status->setToolTip(entry.error);
    table_->setItem(row, 3, status);
  }

  button_cancel_->setEnabled(has_running);
  button_retry_->setEnabled(has_failed);

}

void YouTubeMusicDownloadDialog::CancelClicked() {

  if (service_) service_->CancelCurrentDownload();

}

void YouTubeMusicDownloadDialog::RetryClicked() {

  if (service_) service_->RetryFailedDownloads();

}
