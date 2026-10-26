/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICDOWNLOADDIALOG_H
#define YOUTUBEMUSICDOWNLOADDIALOG_H

#include "config.h"

#include <QDialog>

#include "includes/shared_ptr.h"

class QTableWidget;
class QPushButton;
class YouTubeMusicService;

// Shows the download queue: one row per requested song with its state and progress,
// plus cancel / retry actions. Fed by YouTubeMusicService::DownloadQueueChanged().
class YouTubeMusicDownloadDialog : public QDialog {
  Q_OBJECT

 public:
  explicit YouTubeMusicDownloadDialog(SharedPtr<YouTubeMusicService> service, QWidget *parent = nullptr);
  ~YouTubeMusicDownloadDialog() override;

 private Q_SLOTS:
  void Refresh();
  void CancelClicked();
  void RetryClicked();

 private:
  SharedPtr<YouTubeMusicService> service_;
  QTableWidget *table_;
  QPushButton *button_cancel_;
  QPushButton *button_retry_;
};

#endif  // YOUTUBEMUSICDOWNLOADDIALOG_H
