/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICINDEXLOGIC_H
#define YOUTUBEMUSICINDEXLOGIC_H

#include "config.h"

#include <QHash>
#include <QList>
#include <QString>

#include "core/song.h"

// The decisions the library indexer makes, kept out of the service so they can be tested without
// a database, a network or a session. Both of them have failed in ways that were invisible:
// a wrong order silently re-indexed the wrong artists, and the wrong artist name split the tree
// into 1264 artists instead of 145.
namespace YouTubeMusicIndexLogic {

struct Candidate {
  QString id;
  QString name;
};

// Artists that need walking, least recently indexed first (never indexed before everything else),
// capped at max_artists when that is greater than zero.
QList<Candidate> StaleArtists(const QList<Candidate> &candidates, const QHash<QString, qint64> &stamps,
                              const qint64 now, const qint64 ttl_seconds, const int max_artists = 0);

// True when an artist needs walking: no stamp, or older than the TTL.
bool IsStale(const QString &artist_id, const QHash<QString, qint64> &stamps, const qint64 now, const qint64 ttl_seconds);

// Applies the album row a walk used to the tracks its page returned: the album name and id come
// from the album row when the track has none, and the artist is the one whose walk produced the
// album - never the album's credit line, which is how "Rezz & PVRIS" became an artist.
void ApplyAlbumContext(SongList &tracks, const Song &album, const QString &walked_artist);

}  // namespace YouTubeMusicIndexLogic

#endif  // YOUTUBEMUSICINDEXLOGIC_H
