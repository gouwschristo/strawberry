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
#include "youtubemusicindexlogic.h"

#include <algorithm>

#include <QHash>
#include <QList>
#include <QString>

#include "core/song.h"

namespace YouTubeMusicIndexLogic {

bool IsStale(const QString &artist_id, const QHash<QString, qint64> &stamps, const qint64 now, const qint64 ttl_seconds) {

  const auto stamp = stamps.constFind(artist_id);
  if (stamp == stamps.cend()) return true;

  return now - stamp.value() >= ttl_seconds;

}

QList<Candidate> StaleArtists(const QList<Candidate> &candidates, const QHash<QString, qint64> &stamps,
                              const qint64 now, const qint64 ttl_seconds, const int max_artists) {

  QList<Candidate> stale;
  for (const Candidate &candidate : candidates) {
    if (IsStale(candidate.id, stamps, now, ttl_seconds)) stale << candidate;
  }

  // Least recently indexed first; artists with no stamp at all come first (`value()` returns 0).
  std::stable_sort(stale.begin(), stale.end(), [&stamps](const Candidate &a, const Candidate &b) {
    return stamps.value(a.id) < stamps.value(b.id);
  });

  if (max_artists > 0 && stale.count() > max_artists) stale = stale.mid(0, max_artists);

  return stale;

}

void ApplyAlbumContext(SongList &tracks, const Song &album, const QString &walked_artist) {

  const QString artist = walked_artist.isEmpty() ? album.artist() : walked_artist;

  for (Song &track : tracks) {
    if (track.album().isEmpty() && !album.album().isEmpty()) track.set_album(album.album());
    if (track.album_id().isEmpty() && !album.album_id().isEmpty()) track.set_album_id(album.album_id());
    if (!artist.isEmpty()) {
      track.set_artist(artist);
      track.set_albumartist(artist);
    }
  }

}

}  // namespace YouTubeMusicIndexLogic
