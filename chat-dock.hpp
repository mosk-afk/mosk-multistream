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
#include <QMap>
#include <QStringList>
#include <atomic>
#include <functional>
#include <mutex>
#include <set>
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
	// (Re)connects chat to these channels, all at once. Each entry is what the edit fields accept.
	void SetChannels(const QStringList &youtube, const QStringList &kick);
	std::function<QByteArray(const QByteArray &path)> authHandler; // serves /auth/* on the chat server

private:
	static void FrontendEvent(enum obs_frontend_event event, void *param);
	void Start();
	void AddMessages(std::vector<ChatMsg> batch);
	void SetStatus(const QString &key, const QString &text);
	void SetConfig(const QString &key, const QString &value);
	void Broadcast(const QByteArray &json);
	void HandleRequest(QTcpSocket *socket);
	void AddOverlayToScene();
	void YouTubeLoop(std::string input);
	void KickLoop(std::string slug);
	void Post(std::vector<ChatMsg> batch);
	void PostStatus(QString key, QString text);
	void PostConfig(QString key, QString value);

	QLineEdit *ytEdit;
	QLineEdit *kickEdit;
	QPushButton *connectButton;
	QLabel *statusLabel;
	QVBoxLayout *mainLayout;
	QCefWidget *browser = nullptr;
	QMap<QString, QString> status; // per channel

	QTcpServer server;
	QList<QTcpSocket *> clients;
	std::vector<ChatMsg> history;
	QJsonObject config; // {"youtube": [channel ids], "kick": [user ids]} so the page can load 7TV emote sets

	std::atomic<bool> running = false;
	std::mutex socketsMutex;
	std::set<void *> kickSockets; // open websockets, closed by Stop() to unblock their threads
	std::vector<std::thread> threads; // one per channel
};
