/*
 * Strawberry Music Player
 * YouTube Music integration constants
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICCONSTANTS_H
#define YOUTUBEMUSICCONSTANTS_H

namespace YouTubeMusicConstants {

// InnerTube paginates its shelves; these cap how far a single fetch follows them.
constexpr int kLibraryPages = 20;
constexpr int kSearchPages = 5;

// "Load more" raises the cap by this much per activation.
constexpr int kLibraryPagesPerLoadMore = 10;
constexpr int kSearchPagesPerLoadMore = 5;

// Requests kept in flight while walking albums (artist walk, indexer, downloads).
constexpr int kAlbumConcurrency = 3;

// How often the idle prefetcher indexes one stale artist.
constexpr int kPrefetchIntervalSeconds = 3 * 60;

// Stored session cookies younger than this are not re-imported from the browser.
constexpr int kCookieFreshnessSeconds = 30 * 60;

}  // namespace YouTubeMusicConstants

#endif  // YOUTUBEMUSICCONSTANTS_H
