#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include "chat-dock.hpp"
#include "browser-panel.hpp"
#include "net.hpp"
#include <obs-module.h>
#include <util/platform.h>
#include <QDateTime>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <chrono>

#define MAX_HISTORY 100

enum { YOUTUBE = 0, KICK = 1 };

// GET, or POST when json_body is set. Aborts as soon as running goes false.
static bool http(std::atomic<bool> &running, const std::string &url, std::string &out, const std::string &json_body = "")
{
	if (json_body.empty())
		return http_request("GET", url, out, {"Accept-Language: en-US,en"}, "", &running) == 200;
	return http_request("POST", url, out, {"Accept-Language: en-US,en", "Content-Type: application/json"}, json_body,
			    &running) == 200;
}

static void nap(std::atomic<bool> &running, int ms)
{
	for (int i = 0; i < ms / 100 && running; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Text between the first `start` and the next `end`.
static std::string between(const std::string &s, const std::string &start, char end = '"')
{
	size_t p = s.find(start);
	if (p == std::string::npos)
		return {};
	p += start.size();
	size_t e = s.find(end, p);
	return e == std::string::npos ? std::string() : s.substr(p, e - p);
}

static QJsonObject json(const std::string &s)
{
	return QJsonDocument::fromJson(QByteArray::fromStdString(s)).object();
}

static QByteArray compact(const QJsonObject &o)
{
	return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// Video links/ids are used as is; "@handle", "handle", channel URL or "UC..." id go through the channel's /live page.
static std::string youtube_video_id(const std::string &in)
{
	for (const char *marker : {"v=", "youtu.be/", "/live/"}) {
		size_t p = in.find(marker);
		if (p != std::string::npos && in.size() >= p + strlen(marker) + 11)
			return in.substr(p + strlen(marker), 11);
	}
	if (in.size() == 11 && in[0] != '@' && in.find('/') == std::string::npos)
		return in;
	return {};
}

static std::string youtube_live_url(const std::string &in)
{
	if (in.rfind("http", 0) == 0)
		return in.find("/live") == std::string::npos ? in + "/live" : in;
	if (in[0] == '@')
		return "https://www.youtube.com/" + in + "/live";
	if (in.size() == 24 && in.rfind("UC", 0) == 0)
		return "https://www.youtube.com/channel/" + in + "/live";
	return "https://www.youtube.com/@" + in + "/live";
}

static std::string overlay_url(const char *mode)
{
	return "http://127.0.0.1:" + std::to_string(CHAT_PORT) + "/?" + mode;
}

ChatDock::ChatDock(QWidget *parent) : QWidget(parent)
{
	ytEdit = new QLineEdit;
	ytEdit->setPlaceholderText("YouTube: @channel or video link");
	kickEdit = new QLineEdit;
	kickEdit->setPlaceholderText("Kick: username");
	connectButton = new QPushButton("Connect");
	statusLabel = new QLabel;
	statusLabel->setWordWrap(true);
	auto overlayButton = new QPushButton("Add to scene");
	overlayButton->setToolTip(QString::fromStdString("Adds a browser source showing the chat on stream\n" +
							   overlay_url("overlay")));

	auto row = new QHBoxLayout;
	row->addWidget(kickEdit, 1);
	row->addWidget(connectButton);
	auto statusRow = new QHBoxLayout;
	statusRow->addWidget(statusLabel, 1);
	statusRow->addWidget(overlayButton);
	mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(4, 4, 4, 4);
	mainLayout->addWidget(ytEdit);
	mainLayout->addLayout(row);
	mainLayout->addLayout(statusRow);

	connect(connectButton, &QPushButton::clicked, [this] {
		if (running)
			Stop();
		else
			Start();
	});
	connect(overlayButton, &QPushButton::clicked, [this] { AddOverlayToScene(); });

	connect(&server, &QTcpServer::newConnection, [this] {
		while (QTcpSocket *s = server.nextPendingConnection()) {
			connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
			connect(s, &QTcpSocket::readyRead, this, [this, s] { HandleRequest(s); });
		}
	});
	if (!server.listen(QHostAddress::LocalHost, CHAT_PORT))
		statusLabel->setText(QString("Port %1 is busy, chat view unavailable").arg(CHAT_PORT));

	obs_frontend_add_event_callback(FrontendEvent, this);

	char *path = obs_module_config_path("chat.json");
	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	if (data) {
		ytEdit->setText(QString::fromUtf8(obs_data_get_string(data, "youtube")));
		kickEdit->setText(QString::fromUtf8(obs_data_get_string(data, "kick")));
		obs_data_release(data);
	}
	if (!ytEdit->text().isEmpty() || !kickEdit->text().isEmpty())
		Start();
}

ChatDock::~ChatDock()
{
	obs_frontend_remove_event_callback(FrontendEvent, this);
	Stop();
}

void ChatDock::FrontendEvent(enum obs_frontend_event event, void *param)
{
	if (event != OBS_FRONTEND_EVENT_EXIT)
		return;
	auto dock = (ChatDock *)param;
	dock->Stop();
	if (dock->browser) { // must go before obs-browser shuts CEF down
		dock->browser->closeBrowser();
		delete dock->browser;
		dock->browser = nullptr;
	}
}

// obs-browser is only usable after all modules loaded, so this runs from obs_module_post_load.
void ChatDock::InitBrowser()
{
	static QCef *cef = obs_browser_init_panel();
	if (!cef || browser)
		return;
	cef->init_browser();
	cef->wait_for_browser_init();
	browser = cef->create_widget(this, overlay_url("dock"), nullptr);
	mainLayout->addWidget(browser, 1);
}

void ChatDock::AddOverlayToScene()
{
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	obs_source_t *source = obs_get_source_by_name("Mosk Chat");
	if (!source) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "url", overlay_url("overlay").c_str());
		obs_data_set_int(settings, "width", 500);
		obs_data_set_int(settings, "height", 800);
		obs_data_set_string(settings, "css", "body { background-color: rgba(0, 0, 0, 0); margin: 0; overflow: hidden; }");
		source = obs_source_create("browser_source", "Mosk Chat", settings, nullptr);
		obs_data_release(settings);
	}
	if (scene && source)
		obs_scene_add(scene, source);
	obs_source_release(source);
	obs_source_release(scene_source);
}

void ChatDock::Start()
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "youtube", ytEdit->text().trimmed().toUtf8().constData());
	obs_data_set_string(data, "kick", kickEdit->text().trimmed().toUtf8().constData());
	char *path = obs_module_config_path("chat.json");
	obs_data_save_json_safe(data, path, "tmp", "bak");
	bfree(path);
	obs_data_release(data);

	history.clear();
	config = QJsonObject();
	Broadcast(R"({"type":"clear"})");
	status[YOUTUBE].clear();
	status[KICK].clear();
	running = true;

	std::string yt = ytEdit->text().trimmed().toStdString();
	std::string kick = kickEdit->text().trimmed().toLower().toStdString();
	if (kick.find('/') != std::string::npos) // pasted https://kick.com/name
		kick = kick.substr(kick.find_last_of('/') + 1);
	if (!yt.empty())
		ytThread = std::thread(&ChatDock::YouTubeLoop, this, yt);
	if (!kick.empty())
		kickThread = std::thread(&ChatDock::KickLoop, this, kick);

	ytEdit->setEnabled(false);
	kickEdit->setEnabled(false);
	connectButton->setText("Disconnect");
}

void ChatDock::SetChannels(const QString &youtube, const QString &kick)
{
	if (running && ytEdit->text() == youtube && kickEdit->text() == kick)
		return;
	if (running)
		Stop();
	ytEdit->setText(youtube);
	kickEdit->setText(kick);
	Start();
}

void ChatDock::Stop()
{
	running = false;
	if (HINTERNET ws = kickSocket.exchange(nullptr))
		WinHttpCloseHandle(ws); // unblocks WinHttpWebSocketReceive
	if (ytThread.joinable())
		ytThread.join();
	if (kickThread.joinable())
		kickThread.join();

	ytEdit->setEnabled(true);
	kickEdit->setEnabled(true);
	connectButton->setText("Connect");
	statusLabel->setText("Disconnected");
}

void ChatDock::HandleRequest(QTcpSocket *s)
{
	if (!s->canReadLine())
		return;
	QByteArray path = s->readLine().split(' ').value(1);
	disconnect(s, &QTcpSocket::readyRead, this, nullptr);

	if (path.startsWith("/auth/") && authHandler) { // OAuth redirect for the live dock
		QByteArray body = authHandler(path);
		s->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
			 QByteArray::number(body.size()) + "\r\n\r\n" + body);
		s->disconnectFromHost();
		return;
	}

	if (path.startsWith("/events")) {
		s->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
			 "Access-Control-Allow-Origin: *\r\n\r\n");
		QJsonObject c = config;
		c["type"] = "config";
		s->write("data: " + compact(c) + "\n\n");
		for (auto &m : history)
			s->write("data: " + m.json + "\n\n");
		clients.append(s);
		connect(s, &QTcpSocket::disconnected, this, [this, s] { clients.removeAll(s); });
		return;
	}

	QFile file(":/aitum/chat.html");
	file.open(QIODevice::ReadOnly);
	QByteArray body = file.readAll();
	s->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-cache\r\n"
		 "Connection: close\r\nContent-Length: " +
		 QByteArray::number(body.size()) + "\r\n\r\n" + body);
	s->disconnectFromHost();
}

void ChatDock::Broadcast(const QByteArray &json)
{
	for (auto s : clients)
		s->write("data: " + json + "\n\n");
}

void ChatDock::Post(std::vector<ChatMsg> batch)
{
	if (batch.empty())
		return;
	QMetaObject::invokeMethod(
		this, [this, batch = std::move(batch)]() mutable { AddMessages(std::move(batch)); }, Qt::QueuedConnection);
}

void ChatDock::PostStatus(int platform, QString text)
{
	QMetaObject::invokeMethod(this, [this, platform, text] { SetStatus(platform, text); }, Qt::QueuedConnection);
}

void ChatDock::PostConfig(QString key, QString value)
{
	QMetaObject::invokeMethod(this, [this, key, value] { SetConfig(key, value); }, Qt::QueuedConnection);
}

void ChatDock::SetStatus(int platform, const QString &text)
{
	if (!running)
		return;
	status[platform] = text;
	QStringList parts;
	for (auto &s : status)
		if (!s.isEmpty())
			parts << s;
	statusLabel->setText(parts.join("  ·  "));
}

void ChatDock::SetConfig(const QString &key, const QString &value)
{
	if (!running || value.isEmpty() || config[key].toString() == value)
		return;
	config[key] = value;
	QJsonObject c = config;
	c["type"] = "config";
	Broadcast(compact(c));
}

// The page orders by ts itself; history is kept ordered so late joiners get the same view.
void ChatDock::AddMessages(std::vector<ChatMsg> batch)
{
	if (!running)
		return;
	for (auto &m : batch) {
		Broadcast(m.json);
		auto pos = std::upper_bound(history.begin(), history.end(), m.ts,
					    [](qint64 ts, const ChatMsg &x) { return ts < x.ts; });
		history.insert(pos, std::move(m));
	}
	if (history.size() > MAX_HISTORY)
		history.erase(history.begin(), history.end() - MAX_HISTORY);
}

// ponytail: unofficial innertube endpoints (what youtube.com itself uses), no API key or quota.
// Breaks if YouTube changes the live_chat page; upgrade path is the Data API v3 liveChatMessages.list.
void ChatDock::YouTubeLoop(std::string input)
{
	std::string page;
	while (running) {
		std::string vid = youtube_video_id(input), channelId;
		if (vid.empty()) {
			PostStatus(YOUTUBE, "YouTube: looking for live stream...");
			if (http(running, youtube_live_url(input), page)) {
				vid = between(page, "<link rel=\"canonical\" href=\"https://www.youtube.com/watch?v=");
				channelId = between(page, "\"channelId\":\"");
			}
		} else if (http(running, "https://www.youtube.com/watch?v=" + vid, page)) {
			channelId = between(page, "\"channelId\":\"");
		}
		PostConfig("youtube", QString::fromStdString(channelId));

		std::string cont, version;
		if (!vid.empty() && http(running, "https://www.youtube.com/live_chat?is_popout=1&v=" + vid, page)) {
			cont = between(page, "\"continuation\":\"");
			version = between(page, "\"clientVersion\":\"");
		}
		if (cont.empty()) {
			PostStatus(YOUTUBE, "YouTube: offline (retrying)");
			nap(running, 30000);
			continue;
		}
		PostStatus(YOUTUBE, "YouTube: connected");

		while (running && !cont.empty()) {
			std::string body = "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"" + version +
					   "\"}},\"continuation\":\"" + cont + "\"}";
			if (!http(running, "https://www.youtube.com/youtubei/v1/live_chat/get_live_chat?prettyPrint=false",
				  page, body))
				break;
			auto chat = json(page)["continuationContents"].toObject()["liveChatContinuation"].toObject();

			std::vector<ChatMsg> batch;
			for (auto action : chat["actions"].toArray()) {
				auto item = action.toObject()["addChatItemAction"].toObject()["item"].toObject();
				auto r = item["liveChatTextMessageRenderer"].toObject();
				QJsonObject msg{{"type", "msg"}, {"p", "yt"}};
				if (r.isEmpty()) {
					r = item["liveChatPaidMessageRenderer"].toObject();
					if (r.isEmpty())
						continue;
					msg["amount"] = r["purchaseAmountText"].toObject()["simpleText"];
				}
				QJsonArray parts;
				for (auto run : r["message"].toObject()["runs"].toArray()) {
					auto o = run.toObject();
					auto e = o["emoji"].toObject();
					if (o.contains("text"))
						parts.append(QJsonObject{{"t", o["text"]}});
					else if (e["isCustomEmoji"].toBool())
						parts.append(QJsonObject{
							{"e", e["image"].toObject()["thumbnails"].toArray().last().toObject()["url"]},
							{"n", e["shortcuts"].toArray().first()}});
					else
						parts.append(QJsonObject{{"t", e["emojiId"]}});
				}
				QJsonArray badges;
				for (auto b : r["authorBadges"].toArray()) {
					auto br = b.toObject()["liveChatAuthorBadgeRenderer"].toObject();
					auto thumbs = br["customThumbnail"].toObject()["thumbnails"].toArray();
					if (!thumbs.isEmpty())
						badges.append(QJsonObject{{"url", thumbs.last().toObject()["url"]},
									  {"title", br["tooltip"]}});
					else
						badges.append(QJsonObject{{"icon", br["icon"].toObject()["iconType"]},
									  {"title", br["tooltip"]}});
				}
				qint64 ts = r["timestampUsec"].toString().toLongLong() / 1000;
				msg["ts"] = ts;
				msg["name"] = r["authorName"].toObject()["simpleText"];
				msg["parts"] = parts;
				msg["badges"] = badges;
				batch.push_back({ts, compact(msg)});
			}
			Post(std::move(batch));

			cont.clear();
			auto next = chat["continuations"].toArray().first().toObject();
			for (const char *k : {"invalidationContinuationData", "timedContinuationData", "reloadContinuationData"})
				if (next.contains(k))
					cont = next[k].toObject()["continuation"].toString().toStdString();
			nap(running, 3000);
		}
	}
}

static std::wstring wide(const std::string &s)
{
	return QString::fromStdString(s).toStdWString();
}

static void ws_send(HINTERNET ws, const std::string &text)
{
	WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text.data(), (DWORD)text.size());
}

static QByteArray kick_message(const QJsonObject &d)
{
	static const QRegularExpression emote("\\[emote:(\\d+):([^\\]]+)\\]");
	QString content = d["content"].toString();
	QJsonArray parts;
	qsizetype last = 0;
	for (auto it = emote.globalMatch(content); it.hasNext();) {
		auto m = it.next();
		if (m.capturedStart() > last)
			parts.append(QJsonObject{{"t", content.mid(last, m.capturedStart() - last)}});
		parts.append(QJsonObject{{"e", "https://files.kick.com/emotes/" + m.captured(1) + "/fullsize"},
					 {"n", m.captured(2)}});
		last = m.capturedEnd();
	}
	if (last < content.size())
		parts.append(QJsonObject{{"t", content.mid(last)}});

	auto sender = d["sender"].toObject();
	auto identity = sender["identity"].toObject();
	QJsonArray badges;
	for (auto b : identity["badges_v2"].toArray())
		badges.append(QJsonObject{{"url", b.toObject()["image_url"]}, {"title", b.toObject()["name"]}});

	return compact(QJsonObject{
		{"type", "msg"},
		{"p", "kick"},
		{"ts", QDateTime::fromString(d["created_at"].toString(), Qt::ISODate).toMSecsSinceEpoch()},
		{"name", sender["username"]},
		{"color", identity["color"]},
		{"badges", badges},
		{"parts", parts},
	});
}

// Kick chat comes over a public Pusher websocket. Its app key is handed out by web.kick.com, so ask for it.
void ChatDock::KickLoop(std::string slug)
{
	std::string page;
	while (running) {
		PostStatus(KICK, "Kick: connecting...");
		QJsonObject channel;
		if (http(running, "https://kick.com/api/v2/channels/" + slug, page))
			channel = json(page);
		qint64 channelId = channel["id"].toInteger();
		qint64 chatroomId = channel["chatroom"].toObject()["id"].toInteger();
		if (!chatroomId) {
			PostStatus(KICK, "Kick: channel not found (retrying)");
			nap(running, 30000);
			continue;
		}
		PostConfig("kick", QString::number(channel["user_id"].toInteger()));

		std::string key = "34bf7a0ff419a2a775b9", cluster = "us2"; // fallback, seen 2026-10
		if (http(running,
			 "https://web.kick.com/api/v1/realtime/channels/" + std::to_string(channelId) + "/chat/connection",
			 page,
			 R"({"client":{"id":"mosk-multistream","type":"web"},"capabilities":{"accepted_providers":[{"provider":"pusher"}]}})")) {
			for (auto c : json(page)["data"].toObject()["connections"].toArray()) {
				auto creds = c.toObject()["credentials"].toObject();
				if (c.toObject()["provider"].toString() == "pusher" && !creds["app_key"].toString().isEmpty()) {
					key = creds["app_key"].toString().toStdString();
					cluster = creds["cluster"].toString().toStdString();
				}
			}
		}

		HINTERNET session = WinHttpOpen(L"mosk-multistream", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
		HINTERNET conn = session ? WinHttpConnect(session, wide("ws-" + cluster + ".pusher.com").c_str(),
							  INTERNET_DEFAULT_HTTPS_PORT, 0)
					 : nullptr;
		HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET",
							  wide("/app/" + key + "?protocol=7&client=js&version=8.4.0&flash=false").c_str(),
							  nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE)
				     : nullptr;
		HINTERNET ws = nullptr;
		if (req && WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
		    WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
		    WinHttpReceiveResponse(req, nullptr))
			ws = WinHttpWebSocketCompleteUpgrade(req, 0);
		if (req)
			WinHttpCloseHandle(req);

		if (ws) {
			kickSocket = ws;
			ws_send(ws, "{\"event\":\"pusher:subscribe\",\"data\":{\"auth\":\"\",\"channel\":\"chatrooms." +
					    std::to_string(chatroomId) + ".v2\"}}");
			std::string msg;
			char buf[16384];
			while (running) {
				DWORD n = 0;
				WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
				if (WinHttpWebSocketReceive(ws, buf, sizeof(buf), &n, &type) != NO_ERROR ||
				    type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
					break;
				msg.append(buf, n);
				if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
				    type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
					continue;

				auto ev = json(msg);
				msg.clear();
				QString event = ev["event"].toString();
				if (event == "pusher:ping") {
					ws_send(ws, "{\"event\":\"pusher:pong\",\"data\":{}}");
				} else if (event == "pusher_internal:subscription_succeeded") {
					PostStatus(KICK, "Kick: connected");
				} else if (event == "App\\Events\\ChatMessageEvent") {
					auto d = QJsonDocument::fromJson(ev["data"].toString().toUtf8()).object();
					qint64 ts = QDateTime::fromString(d["created_at"].toString(), Qt::ISODate).toMSecsSinceEpoch();
					Post({{ts, kick_message(d)}});
				}
			}
		}
		if (HINTERNET h = kickSocket.exchange(nullptr))
			WinHttpCloseHandle(h);
		if (conn)
			WinHttpCloseHandle(conn);
		if (session)
			WinHttpCloseHandle(session);

		if (running) {
			PostStatus(KICK, "Kick: disconnected (retrying)");
			nap(running, 5000);
		}
	}
}
