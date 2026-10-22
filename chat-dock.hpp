#pragma once

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QWidget>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

struct ChatMsg {
	qint64 ts; // ms since epoch, used to merge both platforms in time order
	QString html;
};

// Read-only YouTube + Kick chat merged into one list.
class ChatDock : public QWidget {
	Q_OBJECT

public:
	ChatDock(QWidget *parent = nullptr);
	~ChatDock();
	void Stop();

private:
	void Start();
	void AddMessages(std::vector<ChatMsg> batch);
	void SetStatus(int platform, const QString &text);
	void YouTubeLoop(std::string input);
	void KickLoop(std::string slug);
	void Post(std::vector<ChatMsg> batch);
	void PostStatus(int platform, QString text);

	QLineEdit *ytEdit;
	QLineEdit *kickEdit;
	QPushButton *connectButton;
	QLabel *statusLabel;
	QTextBrowser *view;
	QString status[2];

	std::vector<ChatMsg> msgs;
	std::atomic<bool> running = false;
	std::atomic<void *> kickSocket = nullptr;
	std::thread ytThread;
	std::thread kickThread;
};
