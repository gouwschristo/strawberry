/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICPARSER_H
#define YOUTUBEMUSICPARSER_H

#include "config.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

// The pure parts of reading an InnerTube reply: walking the renderer tree, reading a text object,
// finding a row's video id and reading an album's release year out of the header. Kept apart from
// the request class so the shapes that keep changing (album rows served as "playlist" items,
// "Album - 2004" headers) can be tested against captured JSON without a network or a session.
namespace YouTubeMusicParser {

using namespace Qt::Literals::StringLiterals;


inline void CollectItems(const QJsonValue &value, QList<QJsonObject> &items, const bool list_rows_only = false) {

  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    if (object.contains("musicResponsiveListItemRenderer"_L1)) {
      items << object["musicResponsiveListItemRenderer"_L1].toObject();
      return;
    }
    if (object.contains("musicTwoRowItemRenderer"_L1)) {
      if (!list_rows_only) items << object["musicTwoRowItemRenderer"_L1].toObject();
      return;
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
      CollectItems(it.value(), items, list_rows_only);
    }
  }
  else if (value.isArray()) {
    const QJsonArray array = value.toArray();
    for (const QJsonValue &item : array) {
      CollectItems(item, items, list_rows_only);
    }
  }

}

inline QStringList TextRuns(const QJsonValue &value) {

  QStringList ret;

  if (!value.isObject()) return ret;

  const QJsonObject object = value.toObject();
  if (object.contains("simpleText"_L1)) {
    ret << object["simpleText"_L1].toString();
    return ret;
  }

  const QJsonValue runs = object["runs"_L1];
  if (runs.isArray()) {
    const QJsonArray array = runs.toArray();
    for (const QJsonValue &run : array) {
      ret << run.toObject()["text"_L1].toString();
    }
  }

  return ret;

}

inline void CollectObjectsWithKey(const QJsonValue &value, const QString &key, QList<QJsonObject> &objects) {

  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    const QJsonValue found = object[key];
    if (found.isObject()) {
      objects << found.toObject();
      return;
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
      CollectObjectsWithKey(it.value(), key, objects);
    }
  }
  else if (value.isArray()) {
    const QJsonArray array = value.toArray();
    for (const QJsonValue &item : array) {
      CollectObjectsWithKey(item, key, objects);
    }
  }

}

inline int AlbumYearFromHeader(const QJsonObject &json_object) {

  static const QRegularExpression year_re(u"^\\d{4}$"_s);

  QList<QJsonObject> headers;
  CollectObjectsWithKey(json_object, u"musicResponsiveHeaderRenderer"_s, headers);
  for (const QJsonObject &header : std::as_const(headers)) {
    const QJsonArray runs = header[u"subtitle"_s].toObject()[u"runs"_s].toArray();
    for (const QJsonValue &run : runs) {
      const QString text = run.toObject()[u"text"_s].toString().trimmed();
      if (!year_re.match(text).hasMatch()) continue;
      const int year = text.toInt();
      if (year > 1000) return year;
    }
  }

  return -1;

}

inline QString VideoId(const QJsonValue &item) {

  QList<QJsonObject> watch_endpoints;
  CollectObjectsWithKey(item, u"watchEndpoint"_s, watch_endpoints);

  for (const QJsonObject &watch_endpoint : watch_endpoints) {
    const QString video_id = watch_endpoint["videoId"_L1].toString();
    if (!video_id.isEmpty()) return video_id;
  }

  // Album and playlist rows served in the "playlist" shape carry the id only in
  // playlistItemData (navigationEndpoint is null). Those rows were skipped, which left whole
  // singles and community albums without any tracks.
  QList<QJsonObject> playlist_items;
  CollectObjectsWithKey(item, u"playlistItemData"_s, playlist_items);
  for (const QJsonObject &playlist_item : playlist_items) {
    const QString video_id = playlist_item["videoId"_L1].toString();
    if (!video_id.isEmpty()) return video_id;
  }

  return QString();

}

}  // namespace YouTubeMusicParser

#endif  // YOUTUBEMUSICPARSER_H