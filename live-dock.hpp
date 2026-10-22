#pragma once

#include <obs-frontend-api.h>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>
#include <QWidget>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class ChatDock;
class MultistreamDock;

struct Account {
	QString platform; // "yt" or "kick"
	QString name;     // channel title / Kick slug, shown in the dock
	QString channel;  // YouTube channel id or Kick slug
	QString access, refresh;
	qint64 expires = 0; // epoch seconds
	QString title;
	QString privacy = "public"; // YouTube broadcast: public, unlisted or private
	bool live = true;     // included when pressing Go Live
	QString streamId;     // YouTube liveStream reused for every broadcast
	QString broadcastId;  // current YouTube broadcast, for title edits
	QString scheduled;    // its scheduledStartTime, required when updating the title
};

// Linked YouTube/Kick accounts: OAuth login, Go Live (fetches ingest + sets titles, then starts the main
// stream and Multistream outputs), title edits while live, and pointing the chat dock at the live channels.
class LiveDock : public QWidget {
	Q_OBJECT

public:
	LiveDock(QWidget *parent, ChatDock *chat, MultistreamDock *multistream);
	~LiveDock();

private:
	static void FrontendEvent(enum obs_frontend_event event, void *param);
	QByteArray HandleAuth(const QByteArray &path);
	void Load();
	void Save();
	void Rebuild();
	void Login(const QString &platform);
	void EditKeys();
	void GoLive();
	void UpdateTitles();
	void EndLive();
	void Run(std::function<void()> work);
	void SetBusy(bool busy, const QString &status);
	void Merge(const std::vector<Account> &updated);

	ChatDock *chat;
	MultistreamDock *multistream;
	QVBoxLayout *accountsLayout;
	QLabel *statusLabel;
	QPushButton *liveButton, *titleButton, *endButton;
	std::vector<QLineEdit *> titleEdits;
	std::vector<QCheckBox *> liveChecks;
	std::vector<QComboBox *> privacyBoxes; // nullptr for Kick rows

	QString googleId, googleSecret, kickId, kickSecret;
	std::vector<Account> accounts;
	QString pendingState[2], pendingVerifier[2]; // OAuth in flight, indexed yt=0 kick=1
	std::vector<std::string> startedOutputs;     // Multistream outputs we started, stopped on End
	QSet<QString> liveIds;                       // "platform:channel" of accounts streaming right now
	QString chatYt, chatKick;                    // what the chat dock is following
	bool waitingForMain = false;

	std::thread worker;
	std::atomic<bool> running = true;
};
