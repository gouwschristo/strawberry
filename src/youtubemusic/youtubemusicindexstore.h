/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICINDEXSTORE_H
#define YOUTUBEMUSICINDEXSTORE_H

#include "config.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include "includes/shared_ptr.h"

class Database;

// The SQL the indexer and the startup cleanup run, kept out of the service so it can be exercised
// against a real (in-memory) database: the subscription query returning the wrong thing, or the
// delete matching too much, are exactly the mistakes that have consequences here.
namespace YouTubeMusicStore {

// Stamps: artist id -> unix seconds it was last walked.
QHash<QString, qint64> ReadStamps(const SharedPtr<Database> &db);
bool WriteStamp(const SharedPtr<Database> &db, const QString &artist_id, const qint64 indexed_at);

// Every distinct artist name in the tree.
QStringList TreeNames(const SharedPtr<Database> &db);

// The subscription list. The liked-artists shelf is stored as rows with neither an album nor a
// song; the artists of liked songs live in a different table entirely, which is what made the
// first version of the cleanup fold the wrong names.
QStringList SubscribedNames(const SharedPtr<Database> &db);

// Rows belonging to one artist name. Returns the number of rows removed, or -1 on error.
int RemoveArtist(const SharedPtr<Database> &db, const QString &artist);

// Album rows whose album has no tracks at all (they would open to nothing). -1 on error.
int RemoveAlbumsWithoutTracks(const SharedPtr<Database> &db);

}  // namespace YouTubeMusicStore

#endif  // YOUTUBEMUSICINDEXSTORE_H
