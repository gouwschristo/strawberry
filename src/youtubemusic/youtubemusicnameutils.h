/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICNAMEUTILS_H
#define YOUTUBEMUSICNAMEUTILS_H

#include "config.h"

#include <QHash>
#include <QString>
#include <QStringList>

// Name handling for the artist tree: YouTube Music exposes collaborations as artists of their own
// ("The Chainsmokers & Coldplay"), and a name only folds into a subscribed artist when one of its
// parts is subscribed, so real band names ("Chase & Status", "Of Mice & Men") stay untouched.
// Kept free of Qt widgets and the database so it can be tested directly.
namespace YouTubeMusicNames {

// True when a name looks like a collaboration: contains "&", "feat", a standalone "x" or a comma.
bool LooksLikeCollaboration(const QString &name);

// The pieces of a collaboration name, trimmed. Empty when there is nothing to split on.
QStringList SplitCollaboration(const QString &name);

// The subscribed name contained in this one, preferring the longest ("Becky Hill & Chase & Status"
// must resolve to "Chase & Status", not to "Chase"). `subscribed` maps lower-case name to the
// canonical spelling. Empty when nothing matches.
QString BestSubscribedMatch(const QString &name, const QHash<QString, QString> &subscribed);

// The subscribed artist a row should be folded into, or empty when it should be left alone.
// A name that is itself subscribed is never folded.
QString FoldTarget(const QString &name, const QHash<QString, QString> &subscribed);

}  // namespace YouTubeMusicNames

#endif  // YOUTUBEMUSICNAMEUTILS_H
