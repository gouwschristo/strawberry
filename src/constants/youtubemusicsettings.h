/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICSETTINGS_H
#define YOUTUBEMUSICSETTINGS_H

namespace YouTubeMusicSettings {

constexpr char kSettingsGroup[] = "YouTubeMusic";

constexpr char kEnabled[] = "enabled";
constexpr char kCookie[] = "cookie";
constexpr char kYtdlpPath[] = "ytdlp_path";
constexpr char kSearchLimit[] = "searchlimit";
constexpr char kRemoveRemastered[] = "remove_remastered";
constexpr char kAutoRefreshCookies[] = "auto_refresh_cookies";
constexpr char kCookiesScript[] = "cookies_script";
constexpr char kDownloadDir[] = "download_dir";
constexpr char kDownloadFormat[] = "download_format";
constexpr char kDownloadEmbedThumbnail[] = "download_embed_thumbnail";

}  // namespace YouTubeMusicSettings

#endif  // YOUTUBEMUSICSETTINGS_H
