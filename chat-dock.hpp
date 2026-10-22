#pragma once

#include <obs-frontend-api.h>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVBoxLayout>
#include <QWidget>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class QCefWidget;

#define CHAT_PORT 4466

struct ChatMsg {
	qint64 ts; // ms since epoch, used to merge both platforms in time order
	QByteArray json;
};

// Read-only YouTube + Kick chat. Messages are served as Server-Sent Events from 127.0.0.1:CHAT_PORT
// to data/chat.html, which renders them in this dock and in the stream overlay browser source.
class ChatDock : public QWidget {
	Q_OBJECT

public:
	ChatDock(QWidget *parent = nullptr);
	~ChatDock();
	void InitBrowser();
	void Stop();
	void SetChannels(const QString &youtube, const QString &kick); // (re)connects chat to these
	std::function<QByteArray(const QByteArray &path)> authHandler;   // serves /auth/* on the chat server

private:
	static void FrontendEvent(enum obs_frontend_event event, void *param);
	void Start();
	void AddMessages(std::vector<ChatMsg> batch);
	void SetStatus(int platform, const QString &text);
	void SetConfig(const QString &key, const QString &value);
	void Broadcast(const QByteArray &json);
	void HandleRequest(QTcpSocket *socket);
	void AddOverlayToScene();
	void YouTubeLoop(std::string input);
	void KickLoop(std::string slug);
	void Post(std::vector<ChatMsg> batch);
	void PostStatus(int platform, QString text);
	void PostConfig(QString key, QString value);

	QLineEdit *ytEdit;
	QLineEdit *kickEdit;
	QPushButton *connectButton;
	QLabel *statusLabel;
	QVBoxLayout *mainLayout;
	QCefWidget *browser = nullptr;
	QString status[2];

	QTcpServer server;
	QList<QTcpSocket *> clients;
	std::vector<ChatMsg> history;
	QJsonObject config; // platform ids the page needs to load 7TV emote sets

	std::atomic<bool> running = false;
	std::atomic<void *> kickSocket = nullptr;
	std::thread ytThread;
	std::thread kickThread;
};
