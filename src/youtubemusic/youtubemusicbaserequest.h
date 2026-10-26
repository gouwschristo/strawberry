/*
 * Strawberry Music Player
 * YouTube Music integration
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef YOUTUBEMUSICBASEREQUEST_H
#define YOUTUBEMUSICBASEREQUEST_H

#include "config.h"

#include <QByteArray>
#include <QString>
#include <QJsonObject>

#include "includes/shared_ptr.h"
#include "core/jsonbaserequest.h"

class QNetworkReply;
class NetworkAccessManager;
class YouTubeMusicService;

class YouTubeMusicBaseRequest : public JsonBaseRequest {
  Q_OBJECT

 public:
  explicit YouTubeMusicBaseRequest(YouTubeMusicService *service, const SharedPtr<NetworkAccessManager> network, QObject *parent = nullptr);

  // The InnerTube client context shared by every request.
  static QJsonObject CreateContext();

 protected:
  QString service_name() const override;
  bool authentication_required() const override;
  bool authenticated() const override;
  bool use_authorization_header() const override;
  QByteArray authorization_header() const override;

  QNetworkReply *CreateInnerTubeRequest(const QString &endpoint, const QJsonObject &json_body);
  JsonObjectResult ParseJsonObject(QNetworkReply *reply);

  YouTubeMusicService *service_;
  const SharedPtr<NetworkAccessManager> network_;
  QNetworkReply *reply_;
};

#endif  // YOUTUBEMUSICBASEREQUEST_H
