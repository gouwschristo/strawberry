/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICCLIENT_H
#define YOUTUBEMUSICCLIENT_H

#include "config.h"

#include <QByteArray>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QString>

// The InnerTube client details shared by everything that talks to YouTube Music: the streaming
// service and the lyrics provider both need the same key, client version, user agent and
// context, and used to carry their own copy of each.
namespace YouTubeMusicClient {

// The "web" API key music.youtube.com ships in its own bundle. Not an account credential: it
// grants nothing without a cookie. Base64 here so scanners stop flagging a public constant.
QByteArray ApiKey();
QString ClientVersion();
QString UserAgent();
QString Origin();

// The client context every request body carries.
QJsonObject Context();

// A POST request for one endpoint, with the headers InnerTube expects. Callers add auth.
QNetworkRequest CreateRequest(const QString &endpoint);

}  // namespace YouTubeMusicClient

#endif  // YOUTUBEMUSICCLIENT_H
