/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICSETTINGSPAGE_H
#define YOUTUBEMUSICSETTINGSPAGE_H

#include "config.h"

#include <QObject>
#include <QString>
#include <QWidget>

#include "includes/shared_ptr.h"
#include "settings/settingspage.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QSpinBox;
class SettingsDialog;
class YouTubeMusicService;

class YouTubeMusicSettingsPage : public SettingsPage {
  Q_OBJECT

 public:
  explicit YouTubeMusicSettingsPage(SettingsDialog *dialog, SharedPtr<YouTubeMusicService> service, QWidget *parent = nullptr);
  ~YouTubeMusicSettingsPage() override;

  void Load() override;
  void Save() override;

 private Q_SLOTS:
  void CookieChanged();

 private:
  void UpdateLoginState();

  SharedPtr<YouTubeMusicService> service_;

  QCheckBox *checkbox_enabled_;
  QPlainTextEdit *textedit_cookie_;
  QLabel *label_login_state_;
  QCheckBox *checkbox_auto_refresh_cookies_;
  QLineEdit *lineedit_cookies_script_;
  QLineEdit *lineedit_ytdlp_;
  QSpinBox *spinbox_search_limit_;
  QCheckBox *checkbox_remove_remastered_;
  QLineEdit *lineedit_download_dir_;
  QComboBox *combobox_download_format_;
  QCheckBox *checkbox_embed_thumbnail_;
};

#endif  // YOUTUBEMUSICSETTINGSPAGE_H
