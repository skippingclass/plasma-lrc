// SPDX-License-Identifier: MIT
#pragma once
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <functional>

// Loopback-only HTTP fixture: no real lyric service is contacted.
class HttpTestServer : public QTcpServer
{
public:
    struct Response {
        int status = 200;
        QByteArray body = "[]";
        int delayMs = 0;
        QByteArray extraHeaders;
    };
    QList<Response> responses;
    QStringList requests;
    HttpTestServer()
    {
        if (!listen(QHostAddress::LocalHost)) qFatal("Could not start loopback HTTP fixture");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, bytes = QByteArray(), handled = false]() mutable {
                    bytes += socket->readAll();
                    if (handled || !bytes.contains("\r\n\r\n")) return;
                    handled = true;
                    requests.append(QString::fromUtf8(bytes.left(bytes.indexOf("\r\n")).split(' ').value(1)));
                    const Response response = responses.isEmpty() ? Response{} : responses.takeFirst();
                    QTimer::singleShot(response.delayMs, socket, [socket, response] {
                        socket->write("HTTP/1.1 " + QByteArray::number(response.status) + " Response\r\n"
                                      "Content-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                      + QByteArray::number(response.body.size()) + "\r\n" + response.extraHeaders
                                      + "\r\n" + response.body);
                        socket->disconnectFromHost();
                    });
                });
            }
        });
    }
    QUrl apiBase() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/api/").arg(serverPort())); }
};

inline bool waitFor(const std::function<bool()> &condition, int timeoutMs = 2000)
{
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}
inline void advanceEvents(int milliseconds)
{
    QElapsedTimer clock;
    clock.start();
    waitFor([&] { return clock.elapsed() >= milliseconds; }, milliseconds + 100);
}
