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
      textedit_cookie_(new QPlainTextEdit(this)),
      label_login_state_(new QLabel(this)),
      checkbox_auto_refresh_cookies_(new QCheckBox(tr("Refresh the cookies automatically when the session expires"), this)),
      lineedit_cookies_script_(new QLineEdit(this)),
      lineedit_ytdlp_(new QLineEdit(this)),
      spinbox_search_limit_(new QSpinBox(this)),
      checkbox_remove_remastered_(new QCheckBox(tr("Remove \"Remastered\" from song titles"), this)),
      lineedit_download_dir_(new QLineEdit(this)),
      combobox_download_format_(new QComboBox(this)),
      checkbox_embed_thumbnail_(new QCheckBox(tr("Embed cover art in downloaded files"), this)) {

  setWindowIcon(IconLoader::Load(u"youtubemusic"_s, true, 0, 32));

  QVBoxLayout *layout = new QVBoxLayout(this);

  layout->addWidget(checkbox_enabled_);

  QGroupBox *group_login = new QGroupBox(tr("Account"), this);
  QVBoxLayout *layout_login = new QVBoxLayout(group_login);

  QLabel *label_instructions = new QLabel(
      tr("YouTube Music has no public API, so the app authenticates with the cookies of a browser "
         "session. Log in to <a href=\"https://music.youtube.com\">music.youtube.com</a> in your browser, "
         "open the developer tools, and copy the <b>Cookie</b> request header (or just the "
         "<b>SAPISID</b> and <b>__Secure-3PAPISID</b> values) here. Without cookies, searching and "
         "playing songs works, but your library, liked artists and liked albums won't be listed."),
      group_login);
  label_instructions->setWordWrap(true);
  label_instructions->setOpenExternalLinks(true);
  layout_login->addWidget(label_instructions);

  textedit_cookie_->setPlaceholderText(u"SAPISID=...; __Secure-3PAPISID=..."_s);
  textedit_cookie_->setFixedHeight(70);
  layout_login->addWidget(textedit_cookie_);

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

  QObject::connect(textedit_cookie_, &QPlainTextEdit::textChanged, this, &YouTubeMusicSettingsPage::CookieChanged);

  Load();

}

YouTubeMusicSettingsPage::~YouTubeMusicSettingsPage() {}

void YouTubeMusicSettingsPage::Load() {

  Settings s;
  s.beginGroup(QLatin1String(YouTubeMusicSettings::kSettingsGroup));
  checkbox_enabled_->setChecked(s.value(QLatin1String(YouTubeMusicSettings::kEnabled), true).toBool());
  textedit_cookie_->setPlainText(s.value(QLatin1String(YouTubeMusicSettings::kCookie)).toString());
  lineedit_ytdlp_->setText(s.value(QLatin1String(YouTubeMusicSettings::kYtdlpPath), u"yt-dlp"_s).toString());
  spinbox_search_limit_->setValue(s.value(QLatin1String(YouTubeMusicSettings::kSearchLimit), 50).toInt());
  checkbox_remove_remastered_->setChecked(s.value(QLatin1String(YouTubeMusicSettings::kRemoveRemastered), true).toBool());
  checkbox_auto_refresh_cookies_->setChecked(s.value(QLatin1String(YouTubeMusicSettings::kAutoRefreshCookies), true).toBool());
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
  s.setValue(QLatin1String(YouTubeMusicSettings::kCookie), textedit_cookie_->toPlainText().trimmed());
  s.setValue(QLatin1String(YouTubeMusicSettings::kYtdlpPath), lineedit_ytdlp_->text().trimmed());
  s.setValue(QLatin1String(YouTubeMusicSettings::kSearchLimit), spinbox_search_limit_->value());
  s.setValue(QLatin1String(YouTubeMusicSettings::kRemoveRemastered), checkbox_remove_remastered_->isChecked());
  s.setValue(QLatin1String(YouTubeMusicSettings::kAutoRefreshCookies), checkbox_auto_refresh_cookies_->isChecked());
  s.setValue(QLatin1String(YouTubeMusicSettings::kDownloadDir), lineedit_download_dir_->text().trimmed());
  s.setValue(QLatin1String(YouTubeMusicSettings::kDownloadFormat), combobox_download_format_->currentData().toString());
  s.setValue(QLatin1String(YouTubeMusicSettings::kDownloadEmbedThumbnail), checkbox_embed_thumbnail_->isChecked());
  s.setValue(QLatin1String(YouTubeMusicSettings::kCookiesScript), lineedit_cookies_script_->text().trimmed());
  s.endGroup();

  if (service_) service_->ReloadSettings();

  UpdateLoginState();

}

void YouTubeMusicSettingsPage::CookieChanged() {

  UpdateLoginState();

}

void YouTubeMusicSettingsPage::UpdateLoginState() {

  const QString cookie = textedit_cookie_->toPlainText().trimmed();

  if (cookie.isEmpty()) {
    label_login_state_->setText(tr("Not logged in. Only search and playback are available."));
    return;
  }

  if (!cookie.contains("SAPISID"_L1)) {
    label_login_state_->setText(tr("The cookie is missing a SAPISID value, your library won't be accessible."));
    return;
  }

  label_login_state_->setText(tr("Cookies set. Your library will be loaded when the settings are saved."));

}
