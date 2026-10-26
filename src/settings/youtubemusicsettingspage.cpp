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

#include <QDesktopServices>
#include <QUrl>
#include <QApplication>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QVBoxLayout>

#include "includes/shared_ptr.h"
#include "core/logging.h"
#include "core/settings.h"
#include "core/iconloader.h"
#include "constants/youtubemusicsettings.h"
#include "youtubemusic/youtubemusicservice.h"
#include "settingsdialog.h"
#include "youtubemusicsettingspage.h"

using namespace Qt::Literals::StringLiterals;

YouTubeMusicSettingsPage::YouTubeMusicSettingsPage(SettingsDialog *dialog, SharedPtr<YouTubeMusicService> service, QWidget *parent)
    : SettingsPage(dialog, parent),
      service_(service),
      checkbox_enabled_(new QCheckBox(tr("Enable YouTube Music"), this)),
      label_account_state_(new QLabel(this)),
      label_account_detail_(new QLabel(this)),
      button_sign_out_(new QPushButton(tr("Sign out"), this)),
      button_index_library_(new QPushButton(tr("Index library"), this)),
      combobox_cookies_browser_(new QComboBox(this)),
      button_sign_in_(new QPushButton(tr("Sign in to YouTube Music..."), this)),
      button_import_cookies_(new QPushButton(tr("Import cookies now"), this)),
      label_login_state_(new QLabel(this)),
      checkbox_auto_refresh_cookies_(new QCheckBox(tr("Refresh the cookies automatically when the session expires"), this)),
      lineedit_cookies_script_(new QLineEdit(this)),
      lineedit_ytdlp_(new QLineEdit(this)),
      spinbox_search_limit_(new QSpinBox(this)),
      checkbox_remove_remastered_(new QCheckBox(tr("Remove \"Remastered\" from song titles"), this)),
      lineedit_download_dir_(new QLineEdit(this)),
      combobox_download_format_(new QComboBox(this)),
      checkbox_embed_thumbnail_(new QCheckBox(tr("Embed cover art in downloaded files"), this)) {

  // SettingsDialog::AddPage() shows windowTitle() next to windowIcon() in the sidebar.
  setWindowTitle(tr("YouTube Music"));
  setWindowIcon(IconLoader::Load(u"youtubemusic"_s, true, 0, 32));

  QVBoxLayout *layout = new QVBoxLayout(this);

  layout->addWidget(checkbox_enabled_);

  QGroupBox *group_login = new QGroupBox(tr("Account"), this);
  QVBoxLayout *layout_login = new QVBoxLayout(group_login);

  QLabel *label_instructions = new QLabel(
      tr("YouTube Music has no public API, so the app uses the cookie session of a browser. "
         "Sign in to <a href=\"https://music.youtube.com\">music.youtube.com</a> in that browser, then "
         "press <b>Import cookies now</b>. Without a session, searching and playing works, but your "
         "library, liked artists and liked albums won't be listed. You can still paste a cookie header below."),
      group_login);
  label_instructions->setWordWrap(true);
  label_instructions->setOpenExternalLinks(true);
  layout_login->addWidget(label_instructions);

  combobox_cookies_browser_->addItem(tr("Chrome"), u"chrome"_s);
  combobox_cookies_browser_->addItem(tr("Chromium"), u"chromium"_s);
  combobox_cookies_browser_->addItem(tr("Brave"), u"brave"_s);
  combobox_cookies_browser_->addItem(tr("Microsoft Edge"), u"edge"_s);
  combobox_cookies_browser_->addItem(tr("Firefox"), u"firefox"_s);
  combobox_cookies_browser_->addItem(tr("Opera"), u"opera"_s);
  combobox_cookies_browser_->addItem(tr("Vivaldi"), u"vivaldi"_s);
  combobox_cookies_browser_->addItem(tr("Whale"), u"whale"_s);
  combobox_cookies_browser_->setToolTip(tr("Browser to read the session cookies from. Firefox needs no keyring; "
                                           "for Chromium browsers append +gnomekeyring or +kwallet, and a profile "
                                           "with :ProfileName."));
  QFormLayout *layout_browser = new QFormLayout();
  layout_browser->addRow(tr("Read cookies from:"), combobox_cookies_browser_);
  layout_login->addLayout(layout_browser);

  QHBoxLayout *layout_buttons = new QHBoxLayout();
  layout_buttons->addWidget(button_sign_in_);
  layout_buttons->addWidget(button_import_cookies_);
  layout_buttons->addWidget(button_sign_out_);
  layout_buttons->addWidget(button_index_library_);
  layout_buttons->addStretch();
  layout_login->addLayout(layout_buttons);

  label_account_state_->setWordWrap(true);
  QFont state_font = label_account_state_->font();
  state_font.setPointSizeF(state_font.pointSizeF() * 1.15);
  state_font.setBold(true);
  label_account_state_->setFont(state_font);
  layout_login->addWidget(label_account_state_);

  label_account_detail_->setWordWrap(true);
  layout_login->addWidget(label_account_detail_);

  label_login_state_->setWordWrap(true);
  layout_login->addWidget(label_login_state_);

  layout_login->addWidget(checkbox_auto_refresh_cookies_);

  lineedit_cookies_script_->setPlaceholderText(QStringLiteral("%1/Projects/scripts/strawberry-ytm-cookies.py").arg(QDir::homePath()));
  lineedit_cookies_script_->setToolTip(tr("Helper that reads the cookies out of the browser and updates Strawberry's config. "
                                          "It runs when the session expires, so a logged-in browser keeps the library working."));
  QFormLayout *layout_script = new QFormLayout();
  layout_script->addRow(tr("Cookie refresh script:"), lineedit_cookies_script_);
  layout_login->addLayout(layout_script);

  layout->addWidget(group_login);

  QGroupBox *group_playback = new QGroupBox(tr("Playback"), this);
  QFormLayout *layout_playback = new QFormLayout(group_playback);

  lineedit_ytdlp_->setPlaceholderText(u"yt-dlp"_s);
  lineedit_ytdlp_->setToolTip(tr("Streams are resolved with yt-dlp, which must be installed. Leave empty to use \"yt-dlp\" from PATH."));
  layout_playback->addRow(tr("yt-dlp path:"), lineedit_ytdlp_);

  spinbox_search_limit_->setMinimum(5);
  spinbox_search_limit_->setMaximum(200);
  layout_playback->addRow(tr("Maximum results per search:"), spinbox_search_limit_);

  layout_playback->addRow(QString(), checkbox_remove_remastered_);

  layout->addWidget(group_playback);

  QGroupBox *group_downloads = new QGroupBox(tr("Downloads"), this);
  QFormLayout *layout_downloads = new QFormLayout(group_downloads);

  lineedit_download_dir_->setPlaceholderText(QStringLiteral("%1/Music/YouTubeMusic").arg(QDir::homePath()));
  layout_downloads->addRow(tr("Download folder:"), lineedit_download_dir_);

  combobox_download_format_->addItem(tr("Opus (best quality per size)"), u"opus"_s);
  combobox_download_format_->addItem(u"m4a (AAC)"_s, u"m4a"_s);
  combobox_download_format_->addItem(u"mp3"_s, u"mp3"_s);
  combobox_download_format_->addItem(u"flac"_s, u"flac"_s);
  combobox_download_format_->addItem(tr("Keep original format"), u"best"_s);
  layout_downloads->addRow(tr("Audio format:"), combobox_download_format_);

  layout_downloads->addRow(QString(), checkbox_embed_thumbnail_);

  layout->addWidget(group_downloads);
  layout->addStretch();

  QObject::connect(button_sign_in_, &QPushButton::clicked, this, &YouTubeMusicSettingsPage::SignIn);
  QObject::connect(button_import_cookies_, &QPushButton::clicked, this, &YouTubeMusicSettingsPage::ImportCookies);
  QObject::connect(button_sign_out_, &QPushButton::clicked, this, &YouTubeMusicSettingsPage::SignOut);
  QObject::connect(button_index_library_, &QPushButton::clicked, this, &YouTubeMusicSettingsPage::IndexLibrary);

  Load();

}

YouTubeMusicSettingsPage::~YouTubeMusicSettingsPage() {}

void YouTubeMusicSettingsPage::Load() {

  Settings s;
  s.beginGroup(QLatin1String(YouTubeMusicSettings::kSettingsGroup));
  checkbox_enabled_->setChecked(s.value(QLatin1String(YouTubeMusicSettings::kEnabled), true).toBool());
  lineedit_ytdlp_->setText(s.value(QLatin1String(YouTubeMusicSettings::kYtdlpPath), u"yt-dlp"_s).toString());
  spinbox_search_limit_->setValue(s.value(QLatin1String(YouTubeMusicSettings::kSearchLimit), 50).toInt());
  checkbox_remove_remastered_->setChecked(s.value(QLatin1String(YouTubeMusicSettings::kRemoveRemastered), true).toBool());
  checkbox_auto_refresh_cookies_->setChecked(s.value(QLatin1String(YouTubeMusicSettings::kAutoRefreshCookies), true).toBool());
  const QString browser = s.value(QLatin1String(YouTubeMusicSettings::kCookiesBrowser), u"chrome"_s).toString();
  const int browser_index = combobox_cookies_browser_->findData(browser);
  if (browser_index >= 0) combobox_cookies_browser_->setCurrentIndex(browser_index);
  UpdateLoginState();
  lineedit_download_dir_->setText(s.value(QLatin1String(YouTubeMusicSettings::kDownloadDir),
                                          QStringLiteral("%1/Music/YouTubeMusic").arg(QDir::homePath())).toString());
  combobox_download_format_->setCurrentIndex(combobox_download_format_->findData(s.value(QLatin1String(YouTubeMusicSettings::kDownloadFormat), u"opus"_s).toString()));
  checkbox_embed_thumbnail_->setChecked(s.value(QLatin1String(YouTubeMusicSettings::kDownloadEmbedThumbnail), true).toBool());
  lineedit_cookies_script_->setText(s.value(QLatin1String(YouTubeMusicSettings::kCookiesScript),
                                            QStringLiteral("%1/Projects/scripts/strawberry-ytm-cookies.py").arg(QDir::homePath())).toString());
  s.endGroup();

  UpdateLoginState();

  if (!Settings().childGroups().contains(QLatin1String(YouTubeMusicSettings::kSettingsGroup))) set_changed();

}

void YouTubeMusicSettingsPage::Save() {

  Settings s;
  s.beginGroup(QLatin1String(YouTubeMusicSettings::kSettingsGroup));
  s.setValue(QLatin1String(YouTubeMusicSettings::kEnabled), checkbox_enabled_->isChecked());
  s.setValue(QLatin1String(YouTubeMusicSettings::kYtdlpPath), lineedit_ytdlp_->text().trimmed());
  s.setValue(QLatin1String(YouTubeMusicSettings::kSearchLimit), spinbox_search_limit_->value());
  s.setValue(QLatin1String(YouTubeMusicSettings::kRemoveRemastered), checkbox_remove_remastered_->isChecked());
  s.setValue(QLatin1String(YouTubeMusicSettings::kAutoRefreshCookies), checkbox_auto_refresh_cookies_->isChecked());
  s.setValue(QLatin1String(YouTubeMusicSettings::kCookiesBrowser), combobox_cookies_browser_->currentData().toString());
  s.setValue(QLatin1String(YouTubeMusicSettings::kDownloadDir), lineedit_download_dir_->text().trimmed());
  s.setValue(QLatin1String(YouTubeMusicSettings::kDownloadFormat), combobox_download_format_->currentData().toString());
  s.setValue(QLatin1String(YouTubeMusicSettings::kDownloadEmbedThumbnail), checkbox_embed_thumbnail_->isChecked());
  s.setValue(QLatin1String(YouTubeMusicSettings::kCookiesScript), lineedit_cookies_script_->text().trimmed());
  s.endGroup();

  if (service_) service_->ReloadSettings();

  UpdateLoginState();

}

void YouTubeMusicSettingsPage::UpdateLoginState() {

  if (!service_) return;

  const YouTubeMusicService::SessionInfo info = service_->session_info();

  if (!info.signed_in) {
    label_account_state_->setText(tr("Not signed in"));
    label_account_detail_->setText(tr("Search and playback work without a session. Sign in to see your library, "
                                      "liked artists and liked albums."));
    button_sign_out_->setVisible(false);
    return;
  }

  label_account_state_->setText(tr("Signed in"));
  QString detail = tr("Library: %1 songs, %2 artists, %3 albums.")
                     .arg(info.songs).arg(info.artists).arg(info.albums);
  if (!info.imported_at.isEmpty()) {
    detail += u" "_s + (info.browser.isEmpty()
                            ? tr("Session imported %1.").arg(info.imported_at)
                            : tr("Session imported %1 from %2.").arg(info.imported_at, info.browser));
  }
  label_account_detail_->setText(detail);
  button_sign_out_->setVisible(true);

}

void YouTubeMusicSettingsPage::SignIn() {

  // Google refuses its sign-in flow inside embedded browsers, so the login happens in the
  // real browser and the cookies are imported from there.
  QDesktopServices::openUrl(QUrl(u"https://music.youtube.com/"_s));
  label_login_state_->setText(tr("Sign in in the browser, then press \"Import cookies now\"."));

}

void YouTubeMusicSettingsPage::ImportCookies() {

  if (!service_) return;

  const QString browser = combobox_cookies_browser_->currentData().toString();
  const QString old_text = button_import_cookies_->text();
  button_import_cookies_->setEnabled(false);
  button_import_cookies_->setText(tr("Importing..."));
  QApplication::processEvents();

  QString error;
  const bool changed = service_->ImportCookiesFromBrowser(browser, &error);

  button_import_cookies_->setEnabled(true);
  button_import_cookies_->setText(old_text);

  UpdateLoginState();
  if (!changed) {
    label_login_state_->setText(error.isEmpty() ? tr("The cookies did not change.") : error);
  }
  else {
    label_login_state_->clear();
  }

}

void YouTubeMusicSettingsPage::SignOut() {

  if (!service_) return;

  service_->SignOut();
  UpdateLoginState();
  label_login_state_->setText(tr("Signed out. Search and playback still work."));

}

void YouTubeMusicSettingsPage::IndexLibrary() {

  if (!service_) return;

  // Walks every artist whose index entry is stale; progress shows in the status bar and the
  // task manager, and the run is resumable because each artist is stamped when it finishes.
  service_->IndexLibrary();
  label_login_state_->setText(tr("Indexing the library in the background - artists already indexed "
                                 "are skipped."));

}
