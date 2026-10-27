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
#include "youtubemusicnameutils.h"

#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QSet>
#include <QStringList>

using namespace Qt::Literals::StringLiterals;

namespace YouTubeMusicNames {

namespace {
// "&", "feat."/"feat", a standalone "x" (the collaboration separator, not the letter inside a
// word), or a comma. The same separators the tree has always used.
const QRegularExpression &SplitPattern() {
  // "feat." needs its dot inside the match: with a plain \b the dot stayed behind and the next
  // token became ". Ace Ventura" instead of "Ace Ventura".
  static const QRegularExpression pattern(uR"re(\s*(?:&|,|\bx\b|\bfeat\.?(?=\s|$))\s*)re"_s,
                                         QRegularExpression::CaseInsensitiveOption);
  return pattern;
}

// Shorter names produce false matches ("x", "dj") when used as a substring.
constexpr int kMinMatchLength = 3;
}  // namespace

bool LooksLikeCollaboration(const QString &name) {

  return SplitPattern().match(name).hasMatch();

}

QStringList SplitCollaboration(const QString &name) {

  if (!LooksLikeCollaboration(name)) return QStringList();

  QStringList parts;
  for (const QString &part : name.split(SplitPattern(), Qt::SkipEmptyParts)) {
    const QString trimmed = part.trimmed();
    if (!trimmed.isEmpty()) parts << trimmed;
  }

  return parts;

}

QString BestSubscribedMatch(const QString &name, const QHash<QString, QString> &subscribed) {

  QString best_key;
  for (auto it = subscribed.cbegin(); it != subscribed.cend(); ++it) {
    if (it.key().length() < kMinMatchLength) continue;
    if (!name.contains(it.key(), Qt::CaseInsensitive)) continue;
    if (best_key.length() < it.key().length()) best_key = it.key();
  }

  return best_key.isEmpty() ? QString() : subscribed.value(best_key);

}

QString FoldTarget(const QString &name, const QHash<QString, QString> &subscribed) {

  if (name.isEmpty()) return QString();
  if (!LooksLikeCollaboration(name)) return QString();
  // A name that is itself subscribed is a real artist, not a collaboration to fold.
  if (subscribed.contains(name.toLower())) return QString();

  if (const QString contained = BestSubscribedMatch(name, subscribed); !contained.isEmpty()) {
    return contained;
  }

  // No subscribed name contained as a whole: fall back to the individual parts.
  for (const QString &part : SplitCollaboration(name)) {
    const auto it = subscribed.constFind(part.toLower());
    if (it != subscribed.cend()) return it.value();
  }

  return QString();

}

CleanupPlan PlanStrictCleanup(const QStringList &tree_names, const QStringList &subscribed_names) {

  CleanupPlan plan;

  if (subscribed_names.isEmpty()) {
    plan.refused = true;
    plan.reason = u"no subscription list available"_s;
    return plan;
  }

  QSet<QString> subscribed;
  for (const QString &name : subscribed_names) subscribed.insert(name);

  for (const QString &name : tree_names) {
    if (name.isEmpty()) continue;
    if (!subscribed.contains(name)) plan.remove << name;
  }

  // Defence in depth: condemning most of the tree means the subscription list is incomplete,
  // not that the tree is wrong.
  if (!tree_names.isEmpty() && plan.remove.count() * 2 > tree_names.count()) {
    plan.refused = true;
    plan.reason = u"the subscription list looks incomplete"_s;
    plan.remove.clear();
  }

  return plan;

}

}  // namespace YouTubeMusicNames
