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
// Browser (and optional profile / keyring) yt-dlp reads the session cookies from:
// "chrome", "firefox", "brave", "chromium", "edge", "chrome+gnomekeyring:Profile 1", ...
constexpr char kCookiesBrowser[] = "cookies_browser";
constexpr char kCookiesImportedAt[] = "cookies_imported_at";   // ISO date of the last import
// artist id -> ISO date it was last walked by the indexer
constexpr char kIndexedArtists[] = "indexed_artists";
// how long an indexed artist is considered fresh
constexpr int kIndexTtlDays = 7;
// artists the user expanded, most recent first - the prefetcher takes these before the rest
constexpr char kRecentArtists[] = "recent_artists";
// index artists in the background while the app is idle
constexpr char kPrefetchIndex[] = "prefetch_index";
// tidy the artist tree at startup (fold collaborations, drop unsubscribed artists and empty
// albums). Set kStartupCleanupDryRun to see what would be removed without removing it.
constexpr char kStartupCleanup[] = "startup_cleanup";
constexpr char kStartupCleanupDryRun[] = "startup_cleanup_dry_run";
constexpr char kDownloadDir[] = "download_dir";
constexpr char kDownloadFormat[] = "download_format";
constexpr char kDownloadEmbedThumbnail[] = "download_embed_thumbnail";

}  // namespace YouTubeMusicSettings

#endif  // YOUTUBEMUSICSETTINGS_H
