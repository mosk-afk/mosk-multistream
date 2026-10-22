#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include "chat-dock.hpp"
#include <curl/curl.h>
#include <obs-module.h>
#include <util/platform.h>
#include <QColor>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScrollBar>
#include <QVBoxLayout>
#include <algorithm>
#include <chrono>

#define MAX_MESSAGES 300
#define UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36"

enum { YOUTUBE = 0, KICK = 1 };

static size_t write_cb(char *ptr, size_t size, size_t n, void *out)
{
	((std::string *)out)->append(ptr, size * n);
	return size * n;
}

static int abort_cb(void *running, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	return ((std::atomic<bool> *)running)->load() ? 0 : 1;
}

// GET, or POST when json_body is set. Aborts as soon as running goes false.
static bool http(std::atomic<bool> &running, const std::string &url, std::string &out, const std::string &json_body = "")
{
	out.clear();
	CURL *c = curl_easy_init();
	if (!c)
		return false;
	struct curl_slist *headers = curl_slist_append(nullptr, "Accept-Language: en-US,en");
	if (!json_body.empty()) {
		headers = curl_slist_append(headers, "Content-Type: application/json");
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, json_body.c_str());
	}
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, abort_cb);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, &running);
	long code = 0;
	bool ok = curl_easy_perform(c) == CURLE_OK;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(c);
	return ok && code == 200;
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

static QString line(int platform, qint64 ts, const QString &author, const QString &color, const QString &body_html)
{
	static const char *tag[] = {"<b style='color:#ff3d3d'>YT</b>", "<b style='color:#53fc18'>K</b>"};
	return QString("<span style='color:gray'>%1</span> %2 <b style='color:%3'>%4</b>: %5")
		.arg(QDateTime::fromMSecsSinceEpoch(ts).toString("HH:mm"), tag[platform], color, author.toHtmlEscaped(),
		     body_html);
}

// "@handle", "handle", channel URL, "UC..." id -> channel /live page. Video links/ids are handled separately.
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

ChatDock::ChatDock(QWidget *parent) : QWidget(parent)
{
	ytEdit = new QLineEdit;
	ytEdit->setPlaceholderText("YouTube: @channel or video link");
	kickEdit = new QLineEdit;
	kickEdit->setPlaceholderText("Kick: username");
	connectButton = new QPushButton("Connect");
	statusLabel = new QLabel;
	statusLabel->setWordWrap(true);
	view = new QTextBrowser;
	view->setOpenLinks(false);
	view->document()->setMaximumBlockCount(MAX_MESSAGES);

	auto row = new QHBoxLayout;
	row->addWidget(kickEdit, 1);
	row->addWidget(connectButton);
	auto layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->addWidget(ytEdit);
	layout->addLayout(row);
	layout->addWidget(statusLabel);
	layout->addWidget(view, 1);

	connect(connectButton, &QPushButton::clicked, [this] {
		if (running)
			Stop();
		else
			Start();
	});

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
	Stop();
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

	msgs.clear();
	view->clear();
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

void ChatDock::AddMessages(std::vector<ChatMsg> batch)
{
	if (!running)
		return;
	auto bar = view->verticalScrollBar();
	bool atBottom = bar->value() >= bar->maximum() - 4;

	std::stable_sort(batch.begin(), batch.end(), [](const ChatMsg &a, const ChatMsg &b) { return a.ts < b.ts; });
	if (msgs.empty() || batch.front().ts >= msgs.back().ts) {
		// common case: newer than everything shown, just append
		for (auto &m : batch) {
			view->append(m.html);
			msgs.push_back(std::move(m));
		}
	} else {
		// YouTube polls in batches, so its messages can land before Kick ones already shown: re-render in time order
		for (auto &m : batch) {
			auto pos = std::upper_bound(msgs.begin(), msgs.end(), m.ts,
						    [](qint64 ts, const ChatMsg &x) { return ts < x.ts; });
			msgs.insert(pos, std::move(m));
		}
		if (msgs.size() > MAX_MESSAGES)
			msgs.erase(msgs.begin(), msgs.end() - MAX_MESSAGES);
		view->clear();
		for (auto &m : msgs)
			view->append(m.html);
	}
	if (msgs.size() > MAX_MESSAGES)
		msgs.erase(msgs.begin(), msgs.end() - MAX_MESSAGES);
	if (atBottom)
		bar->setValue(bar->maximum());
}

// ponytail: unofficial innertube endpoints (what youtube.com itself uses), no API key or quota.
// Breaks if YouTube changes the live_chat page; upgrade path is the Data API v3 liveChatMessages.list.
void ChatDock::YouTubeLoop(std::string input)
{
	std::string page;
	while (running) {
		std::string vid = youtube_video_id(input);
		if (vid.empty()) {
			PostStatus(YOUTUBE, "YouTube: looking for live stream...");
			if (http(running, youtube_live_url(input), page))
				vid = between(page, "<link rel=\"canonical\" href=\"https://www.youtube.com/watch?v=");
		}
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
				QString prefix;
				if (r.isEmpty()) {
					r = item["liveChatPaidMessageRenderer"].toObject();
					if (r.isEmpty())
						continue;
					prefix = "<b style='color:#ffca28'>[" +
						 r["purchaseAmountText"].toObject()["simpleText"].toString().toHtmlEscaped() +
						 "]</b> ";
				}
				QString text;
				for (auto run : r["message"].toObject()["runs"].toArray()) {
					auto o = run.toObject();
					if (o.contains("text")) {
						text += o["text"].toString();
					} else {
						auto e = o["emoji"].toObject();
						text += e["isCustomEmoji"].toBool() ? e["shortcuts"].toArray().first().toString()
										    : e["emojiId"].toString();
					}
				}
				qint64 ts = r["timestampUsec"].toString().toLongLong() / 1000;
				batch.push_back({ts, line(YOUTUBE, ts, r["authorName"].toObject()["simpleText"].toString(),
							  "#ff8a80", prefix + text.toHtmlEscaped())});
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
			static const QRegularExpression emote("\\[emote:\\d+:([^\\]]+)\\]");
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
					auto sender = d["sender"].toObject();
					QColor color(sender["identity"].toObject()["color"].toString());
					QString text = d["content"].toString().replace(emote, "\\1");
					qint64 ts = QDateTime::fromString(d["created_at"].toString(), Qt::ISODate).toMSecsSinceEpoch();
					Post({{ts, line(KICK, ts, sender["username"].toString(),
							color.isValid() ? color.name() : "#53fc18", text.toHtmlEscaped())}});
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
