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

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QNetworkReply>

#include "includes/shared_ptr.h"
#include "core/logging.h"
#include "core/networkaccessmanager.h"
#include "youtubemusic/youtubemusicservice.h"
#include "youtubemusicbaserequest.h"
#include "youtubemusicclient.h"

using namespace Qt::Literals::StringLiterals;



YouTubeMusicBaseRequest::YouTubeMusicBaseRequest(YouTubeMusicService *service, const SharedPtr<NetworkAccessManager> network, QObject *parent)
    : JsonBaseRequest(network, parent),
      service_(service),
      network_(network),
      reply_(nullptr) {}

QJsonObject YouTubeMusicBaseRequest::CreateContext() {

  // The client the service uses also wants a geo, which the shared context leaves out.
  QJsonObject context = YouTubeMusicClient::Context();
  QJsonObject client = context["client"_L1].toObject();
  client["gl"_L1] = u"US"_s;
  context["client"_L1] = client;

  return context;

}

QString YouTubeMusicBaseRequest::service_name() const {

  return service_->name();

}

// Browse/search works anonymously; only the "your library" endpoints need the
// cookie, so requests are never refused up front.
bool YouTubeMusicBaseRequest::authentication_required() const {

  return false;

}

bool YouTubeMusicBaseRequest::authenticated() const {

  return service_->authenticated();

}

bool YouTubeMusicBaseRequest::use_authorization_header() const {

  return true;

}

QByteArray YouTubeMusicBaseRequest::authorization_header() const {

  return service_->authorization_header();

}

QNetworkReply *YouTubeMusicBaseRequest::CreateInnerTubeRequest(const QString &endpoint, const QJsonObject &json_body) {

  QNetworkRequest network_request = YouTubeMusicClient::CreateRequest(endpoint);
  network_request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

  // Cookie auth only. Measured: with an OAuth Bearer token and no cookie, InnerTube answers
  // "Request contains an invalid argument." for the library endpoints, so the token cannot
  // replace the session cookie here (it is for the Data API, not for music.youtube.com).
  const QString cookie = service_->cookie();
  if (!cookie.isEmpty()) {
    network_request.setRawHeader("Cookie", cookie.toUtf8());
    const QByteArray authorization = authorization_header();
    if (!authorization.isEmpty()) network_request.setRawHeader("Authorization", authorization);
  }

  reply_ = network_->post(network_request, QJsonDocument(json_body).toJson(QJsonDocument::Compact));
  replies_ << reply_;
  QObject::connect(reply_, &QNetworkReply::sslErrors, this, &YouTubeMusicBaseRequest::HandleSSLErrors);

  return reply_;

}

JsonBaseRequest::JsonObjectResult YouTubeMusicBaseRequest::ParseJsonObject(QNetworkReply *reply) {

  if (reply->error() != QNetworkReply::NoError && reply->error() < 200) {
    return ReplyDataResult(ErrorCode::NetworkError, QStringLiteral("%1 (%2)").arg(reply->errorString()).arg(reply->error()));
  }

  JsonObjectResult result(ErrorCode::Success);
  result.network_error = reply->error();
  if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid()) {
    result.http_status_code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  }

  const QByteArray data = reply->readAll();
  if (!data.isEmpty()) {
    QJsonParseError json_parse_error;
    const QJsonDocument json_document = QJsonDocument::fromJson(data, &json_parse_error);
    if (json_parse_error.error == QJsonParseError::NoError) {
      result.json_object = json_document.object();
      // InnerTube reports API errors inside a successful HTTP response.
      const QJsonValue error = result.json_object["error"_L1];
      if (error.isObject()) {
        const QJsonObject error_object = error.toObject();
        result.error_code = ErrorCode::APIError;
        result.api_error = error_object["code"_L1].toInt();
        result.error_message = error_object["message"_L1].toString();
        if (result.error_message.isEmpty()) result.error_message = u"YouTube Music returned an error."_s;
      }
    }
    else {
      result.error_code = ErrorCode::ParseError;
      result.error_message = json_parse_error.errorString();
    }
  }

  if (result.error_code == ErrorCode::Success) {
    if (reply->error() != QNetworkReply::NoError) {
      result.error_code = ErrorCode::NetworkError;
      result.error_message = QStringLiteral("%1 (%2)").arg(reply->errorString()).arg(reply->error());
    }
    else if (result.http_status_code != 200) {
      result.error_code = ErrorCode::HttpError;
      result.error_message = QStringLiteral("Received HTTP code %1").arg(result.http_status_code);
    }
  }

  return result;

}
