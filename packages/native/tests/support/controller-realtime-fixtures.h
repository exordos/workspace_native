#pragma once

#include "native-test-harness.h"

class RealtimeGateway final : public QTcpServer {
public:
    QWebSocketServer websocket{"Controller integration", QWebSocketServer::NonSecureMode};
    QVector<QPointer<QWebSocket>> sockets;
    explicit RealtimeGateway(quint16 httpPort)
    {
        listen(QHostAddress::LocalHost);
        websocket.setSupportedSubprotocols({"workspace.events.v1"});
        connect(this, &QTcpServer::newConnection, this, [this, httpPort] {
            while (auto *client = nextPendingConnection()) {
                connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
                connect(client, &QTcpSocket::readyRead, this, [this, httpPort, client] {
                    if (client->property("routed").toBool())
                        return;
                    const auto headers = client->peek(client->bytesAvailable());
                    if (!headers.contains("\r\n\r\n"))
                        return;
                    client->setProperty("routed", true);
                    if (headers.toLower().contains("upgrade: websocket")) {
                        websocket.handleConnection(client);
                        return;
                    }
                    auto *upstream = new QTcpSocket(client);
                    connect(upstream, &QTcpSocket::connected, client, [client, upstream] { upstream->write(client->readAll()); });
                    connect(client, &QTcpSocket::readyRead, upstream, [client, upstream] {
                        if (upstream->state() == QAbstractSocket::ConnectedState)
                            upstream->write(client->readAll());
                    });
                    connect(upstream, &QTcpSocket::readyRead, client, [client, upstream] { client->write(upstream->readAll()); });
                    connect(upstream, &QTcpSocket::disconnected, client, &QTcpSocket::disconnectFromHost);
                    upstream->connectToHost(QHostAddress::LocalHost, httpPort);
                });
            }
        });
        connect(&websocket, &QWebSocketServer::newConnection, this, [this] {
            auto *socket = websocket.nextPendingConnection();
            sockets.append(socket);
            connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
            socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "ready"},
                {"epoch_generation", "controller-generation"},
                {"epoch_version", QUrlQuery(socket->requestUrl()).queryItemValue("last_epoch_version").toLongLong()}
            }).toJson(QJsonDocument::Compact)));
        });
    }
    QUrl origin() const { return QUrl(QString("http://127.0.0.1:%1").arg(serverPort())); }
};
