#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dpapi.h>

#include "live-dock.hpp"
#include "chat-dock.hpp"
#include "multistream.hpp"
#include "net.hpp"
#include <obs-module.h>
#include <util/platform.h>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

enum { YT = 0, KICK = 1 };

static const char *REDIRECT[] = {"http://127.0.0.1:4466/auth/yt", "http://127.0.0.1:4466/auth/kick"};
static const char *TOKEN_URL[] = {"https://oauth2.googleapis.com/token", "https://id.kick.com/oauth/token"};

struct Keys {
	QString id[2], secret[2];
};

static int index_of(const QString &platform)
{
	return platform == "yt" ? YT : KICK;
}

static QByteArray form(std::initializer_list<std::pair<const char *, QString>> fields)
{
	QByteArray out;
	for (auto &[k, v] : fields) {
		if (!out.isEmpty())
			out += '&';
		out += QByteArray(k) + '=' + QUrl::toPercentEncoding(v);
	}
	return out;
}

static QJsonObject parse(const std::string &s)
{
	return QJsonDocument::fromJson(QByteArray::fromStdString(s)).object();
}

static QString random_string(int n)
{
	static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
	QString s;
	for (int i = 0; i < n; i++)
		s += chars[QRandomGenerator::system()->bounded(int(sizeof(chars) - 1))];
	return s;
}

static QString api_error(long code, const QJsonObject &j)
{
	QString msg = j["error"].isObject() ? j["error"].toObject()["message"].toString()
					     : j["error_description"].toString() + j["message"].toString();
	return QString("HTTP %1 %2").arg(code).arg(msg);
}

// Code exchange or refresh. Fills the account's tokens, returns an error or "".
static QString token_request(Account &a, const QByteArray &body)
{
	std::string out;
	long code = http_request("POST", TOKEN_URL[index_of(a.platform)], out,
				 {"Content-Type: application/x-www-form-urlencoded"}, body.toStdString());
	auto j = parse(out);
	if (code != 200 || j["access_token"].toString().isEmpty())
		return "login: " + api_error(code, j);
	a.access = j["access_token"].toString();
	if (!j["refresh_token"].toString().isEmpty()) // Google only sends it on the first exchange
		a.refresh = j["refresh_token"].toString();
	a.expires = QDateTime::currentSecsSinceEpoch() + j["expires_in"].toInteger(3600);
	return {};
}

static QString fresh(Account &a, const Keys &k)
{
	if (a.expires > QDateTime::currentSecsSinceEpoch() + 60)
		return {};
	int i = index_of(a.platform);
	return token_request(a, form({{"grant_type", "refresh_token"},
				      {"refresh_token", a.refresh},
				      {"client_id", k.id[i]},
				      {"client_secret", k.secret[i]}}));
}

static long api(Account &a, const char *method, const QString &url, QJsonObject &out, const QJsonObject &body = {})
{
	std::vector<std::string> headers{"Authorization: Bearer " + a.access.toStdString(), "Accept: application/json"};
	std::string data, res;
	if (!body.isEmpty()) {
		headers.push_back("Content-Type: application/json");
		data = QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString();
	}
	long code = http_request(method, url.toStdString(), res, headers, data);
	out = parse(res);
	return code;
}

#define YT_API "https://www.googleapis.com/youtube/v3/"
#define KICK_API "https://api.kick.com/public/v1/"

// Where to send video for this account, after setting its title. Runs on the worker thread.
static QString prepare_live(Account &a, std::string &server, std::string &key)
{
	QJsonObject j;
	long code;
	QString title = a.title.isEmpty() ? QString("Live") : a.title;
	if (a.platform == "kick") {
		if ((code = api(a, "GET", KICK_API "channels", j)) != 200)
			return api_error(code, j);
		auto stream = j["data"].toArray().first().toObject()["stream"].toObject();
		server = stream["url"].toString().toStdString();
		key = stream["key"].toString().toStdString();
		if (key.empty())
			return "no stream key returned (is the streamkey:read scope enabled on your Kick app?)";
		if (!a.title.isEmpty() &&
		    (code = api(a, "PATCH", KICK_API "channels", j, {{"stream_title", a.title}})) / 100 != 2)
			return "title: " + api_error(code, j);
		return {};
	}

	// YouTube: one reusable ingest stream per account, a new broadcast (= video) per Go Live.
	QJsonObject stream;
	if (!a.streamId.isEmpty() && api(a, "GET", YT_API "liveStreams?part=cdn&id=" + a.streamId, j) == 200)
		stream = j["items"].toArray().first().toObject();
	if (stream.isEmpty()) {
		QJsonObject body{{"snippet", QJsonObject{{"title", "Mosk Multistream"}}},
				 {"cdn", QJsonObject{{"ingestionType", "rtmp"}, {"resolution", "variable"}, {"frameRate", "variable"}}}};
		if ((code = api(a, "POST", YT_API "liveStreams?part=snippet,cdn", stream, body)) != 200)
			return "stream: " + api_error(code, stream);
		a.streamId = stream["id"].toString();
	}
	auto ingest = stream["cdn"].toObject()["ingestionInfo"].toObject();
	server = ingest["ingestionAddress"].toString().toStdString();
	key = ingest["streamName"].toString().toStdString();

	QJsonObject body{
		{"snippet", QJsonObject{{"title", title},
					{"scheduledStartTime", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}}},
		{"status", QJsonObject{{"privacyStatus", a.privacy}, {"selfDeclaredMadeForKids", false}}},
		{"contentDetails", QJsonObject{{"enableAutoStart", true}, {"enableAutoStop", true}}},
	};
	if ((code = api(a, "POST", YT_API "liveBroadcasts?part=snippet,status,contentDetails", j, body)) != 200)
		return "broadcast: " + api_error(code, j);
	a.broadcastId = j["id"].toString();
	a.scheduled = j["snippet"].toObject()["scheduledStartTime"].toString();
	if ((code = api(a, "POST",
			YT_API "liveBroadcasts/bind?part=id,contentDetails&id=" + a.broadcastId + "&streamId=" + a.streamId,
			j)) != 200)
		return "bind: " + api_error(code, j);
	return {};
}

static QString set_title(Account &a)
{
	QJsonObject j;
	long code;
	if (a.platform == "kick") {
		code = api(a, "PATCH", KICK_API "channels", j, {{"stream_title", a.title}});
		return code / 100 == 2 ? QString() : api_error(code, j);
	}
	if (a.broadcastId.isEmpty()) { // went live some other way: use the active broadcast
		if ((code = api(a, "GET", YT_API "liveBroadcasts?part=snippet&broadcastStatus=active", j)) != 200)
			return api_error(code, j);
		auto item = j["items"].toArray().first().toObject();
		if (item.isEmpty())
			return "no active YouTube broadcast";
		a.broadcastId = item["id"].toString();
		a.scheduled = item["snippet"].toObject()["scheduledStartTime"].toString();
	}
	QJsonObject body{{"id", a.broadcastId},
			 {"snippet", QJsonObject{{"title", a.title}, {"scheduledStartTime", a.scheduled}}}};
	code = api(a, "PUT", YT_API "liveBroadcasts?part=snippet", j, body);
	return code == 200 ? QString() : api_error(code, j);
}

static QString output_name(const Account &a)
{
	return (a.platform == "yt" ? "YouTube: " : "Kick: ") + a.name;
}

// Tokens and app secrets live in accounts.dat, encrypted for the current Windows user (DPAPI).
static QByteArray dpapi(const QByteArray &in, bool encrypt)
{
	DATA_BLOB input{(DWORD)in.size(), (BYTE *)in.data()}, output{};
	BOOL ok = encrypt ? CryptProtectData(&input, L"mosk-multistream", nullptr, nullptr, nullptr, 0, &output)
			  : CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output);
	if (!ok)
		return {};
	QByteArray result((const char *)output.pbData, (int)output.cbData);
	LocalFree(output.pbData);
	return result;
}

static QString accounts_path()
{
	char *path = obs_module_config_path("accounts.dat");
	QString p = QString::fromUtf8(path);
	bfree(path);
	return p;
}

LiveDock::LiveDock(QWidget *parent, ChatDock *chat_, MultistreamDock *multistream_)
	: QWidget(parent),
	  chat(chat_),
	  multistream(multistream_)
{
	accountsLayout = new QVBoxLayout;
	statusLabel = new QLabel;
	statusLabel->setWordWrap(true);
	liveButton = new QPushButton("Go Live");
	titleButton = new QPushButton("Update titles");
	endButton = new QPushButton("End");
	auto addYt = new QPushButton("+ YouTube");
	auto addKick = new QPushButton("+ Kick");
	auto keysButton = new QPushButton("App keys");

	auto addRow = new QHBoxLayout;
	addRow->addWidget(addYt);
	addRow->addWidget(addKick);
	addRow->addWidget(keysButton);
	auto liveRow = new QHBoxLayout;
	liveRow->addWidget(liveButton, 1);
	liveRow->addWidget(titleButton);
	liveRow->addWidget(endButton);

	auto layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->addLayout(accountsLayout);
	layout->addLayout(addRow);
	layout->addLayout(liveRow);
	layout->addWidget(statusLabel);
	layout->addStretch(1);

	connect(addYt, &QPushButton::clicked, [this] { Login("yt"); });
	connect(addKick, &QPushButton::clicked, [this] { Login("kick"); });
	connect(keysButton, &QPushButton::clicked, [this] { EditKeys(); });
	connect(liveButton, &QPushButton::clicked, [this] { GoLive(); });
	connect(titleButton, &QPushButton::clicked, [this] { UpdateTitles(); });
	connect(endButton, &QPushButton::clicked, [this] { EndLive(); });

	chat->authHandler = [this](const QByteArray &path) { return HandleAuth(path); };
	obs_frontend_add_event_callback(FrontendEvent, this);
	Load();
	Rebuild();
	FollowAccounts();
}

// Off air, chat follows the linked channels (ticked ones first); Go Live points it at the live broadcast instead.
void LiveDock::FollowAccounts()
{
	if (accounts.empty() || obs_frontend_streaming_active())
		return;
	QString yt, kick;
	for (int pass = 0; pass < 2; pass++)
		for (auto &a : accounts) {
			if (pass == 0 && !a.live)
				continue;
			if (a.platform == "yt" && yt.isEmpty())
				yt = a.channel;
			if (a.platform == "kick" && kick.isEmpty())
				kick = a.channel;
		}
	chat->SetChannels(yt, kick);
}

LiveDock::~LiveDock()
{
	obs_frontend_remove_event_callback(FrontendEvent, this);
	running = false;
	if (worker.joinable())
		worker.join();
}

void LiveDock::FrontendEvent(enum obs_frontend_event event, void *param)
{
	auto d = (LiveDock *)param;
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED && d->waitingForMain) {
		d->waitingForMain = false;
		d->statusLabel->setText("Live.");
		// Multistream outputs reuse the main stream's encoders, give them a moment to come up.
		QTimer::singleShot(1500, d, [d] {
			for (auto &name : d->startedOutputs)
				d->multistream->SetOutputActive(name.c_str(), true);
		});
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
		d->liveIds.clear();
		d->FollowAccounts();
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		d->running = false;
		if (d->worker.joinable())
			d->worker.join();
	}
}

void LiveDock::Load()
{
	QFile file(accounts_path());
	if (!file.open(QIODevice::ReadOnly))
		return;
	auto j = QJsonDocument::fromJson(dpapi(file.readAll(), false)).object();
	googleId = j["google_id"].toString();
	googleSecret = j["google_secret"].toString();
	kickId = j["kick_id"].toString();
	kickSecret = j["kick_secret"].toString();
	for (auto v : j["accounts"].toArray()) {
		auto o = v.toObject();
		Account a;
		a.platform = o["platform"].toString();
		a.name = o["name"].toString();
		a.channel = o["channel"].toString();
		a.access = o["access"].toString();
		a.refresh = o["refresh"].toString();
		a.expires = o["expires"].toInteger();
		a.title = o["title"].toString();
		a.live = o["live"].toBool(true);
		a.privacy = o["privacy"].toString("public");
		a.streamId = o["stream_id"].toString();
		accounts.push_back(a);
	}
}

void LiveDock::Save()
{
	for (size_t i = 0; i < accounts.size() && i < titleEdits.size(); i++) {
		accounts[i].title = titleEdits[i]->text();
		accounts[i].live = liveChecks[i]->isChecked();
		if (privacyBoxes[i])
			accounts[i].privacy = privacyBoxes[i]->currentData().toString();
	}
	QJsonArray list;
	for (auto &a : accounts)
		list.append(QJsonObject{{"platform", a.platform},
					{"name", a.name},
					{"channel", a.channel},
					{"access", a.access},
					{"refresh", a.refresh},
					{"expires", a.expires},
					{"title", a.title},
					{"live", a.live},
					{"privacy", a.privacy},
					{"stream_id", a.streamId}});
	QJsonObject j{{"google_id", googleId},
		      {"google_secret", googleSecret},
		      {"kick_id", kickId},
		      {"kick_secret", kickSecret},
		      {"accounts", list}};
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	QFile file(accounts_path());
	if (file.open(QIODevice::WriteOnly))
		file.write(dpapi(QJsonDocument(j).toJson(QJsonDocument::Compact), true));
}

void LiveDock::Rebuild()
{
	while (auto item = accountsLayout->takeAt(0)) {
		item->widget()->deleteLater(); // may be the row whose ✕ button is calling us
		delete item;
	}
	titleEdits.clear();
	liveChecks.clear();
	privacyBoxes.clear();
	if (accounts.empty())
		accountsLayout->addWidget(new QLabel("No accounts linked yet."));

	for (size_t i = 0; i < accounts.size(); i++) {
		auto &a = accounts[i];
		auto row = new QWidget;
		auto rowLayout = new QVBoxLayout(row);
		rowLayout->setContentsMargins(0, 2, 0, 2);
		auto top = new QHBoxLayout;
		auto check = new QCheckBox;
		check->setChecked(a.live);
		check->setToolTip("Include in Go Live");
		auto label = new QLabel(QString("<b style='color:%1'>%2</b> %3")
						.arg(a.platform == "yt" ? "#ff3d3d" : "#53fc18",
						     a.platform == "yt" ? "YouTube" : "Kick", a.name.toHtmlEscaped()));
		auto remove = new QPushButton("X");
		remove->setFixedWidth(28);
		remove->setStyleSheet("padding: 0px;"); // OBS theme padding otherwise hides the text
		remove->setToolTip("Unlink account");
		top->addWidget(check);
		top->addWidget(label, 1);
		QComboBox *privacy = nullptr;
		if (a.platform == "yt") {
			privacy = new QComboBox;
			privacy->addItem("Public", "public");
			privacy->addItem("Unlisted", "unlisted");
			privacy->addItem("Private", "private");
			privacy->setCurrentIndex(std::max(0, privacy->findData(a.privacy)));
			privacy->setToolTip("Visibility of the YouTube broadcast created by Go Live");
			top->addWidget(privacy);
			connect(privacy, &QComboBox::currentIndexChanged, [this] { Save(); });
		}
		top->addWidget(remove);
		auto title = new QLineEdit(a.title);
		title->setPlaceholderText("Stream title");
		rowLayout->addLayout(top);
		rowLayout->addWidget(title);
		accountsLayout->addWidget(row);
		titleEdits.push_back(title);
		liveChecks.push_back(check);
		privacyBoxes.push_back(privacy);

		connect(title, &QLineEdit::editingFinished, [this] { Save(); });
		connect(check, &QCheckBox::toggled, [this] { Save(); });
		connect(remove, &QPushButton::clicked, [this, i] {
			if (QMessageBox::question(this, "Unlink account",
						  "Unlink " + accounts[i].name + "?") != QMessageBox::Yes)
				return;
			Save();
			accounts.erase(accounts.begin() + i);
			titleEdits.clear();
			liveChecks.clear();
			privacyBoxes.clear();
			Save();
			Rebuild();
			FollowAccounts();
		});
	}
}

void LiveDock::EditKeys()
{
	QDialog dialog(this);
	dialog.setWindowTitle("App keys");
	auto layout = new QFormLayout(&dialog);
	auto gid = new QLineEdit(googleId), gsecret = new QLineEdit(googleSecret);
	auto kid = new QLineEdit(kickId), ksecret = new QLineEdit(kickSecret);
	gsecret->setEchoMode(QLineEdit::Password);
	ksecret->setEchoMode(QLineEdit::Password);
	auto info = new QLabel(QString("Google: Desktop app OAuth client.<br>Kick redirect URL: <b>%1</b>").arg(REDIRECT[KICK]));
	info->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addRow(info);
	layout->addRow("Google Client ID", gid);
	layout->addRow("Google Client secret", gsecret);
	layout->addRow("Kick Client ID", kid);
	layout->addRow("Kick Client secret", ksecret);
	auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	layout->addRow(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	dialog.resize(520, dialog.sizeHint().height());
	if (dialog.exec() != QDialog::Accepted)
		return;
	googleId = gid->text().trimmed();
	googleSecret = gsecret->text().trimmed();
	kickId = kid->text().trimmed();
	kickSecret = ksecret->text().trimmed();
	Save();
}

void LiveDock::Login(const QString &platform)
{
	int i = index_of(platform);
	if ((i == YT ? googleId : kickId).isEmpty())
		EditKeys();
	QString id = i == YT ? googleId : kickId;
	if (id.isEmpty())
		return;

	pendingVerifier[i] = random_string(64);
	pendingState[i] = random_string(24);
	QString challenge = QCryptographicHash::hash(pendingVerifier[i].toUtf8(), QCryptographicHash::Sha256)
				    .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
	QByteArray query =
		i == YT ? form({{"client_id", id},
				{"redirect_uri", REDIRECT[YT]},
				{"response_type", "code"},
				{"scope", "https://www.googleapis.com/auth/youtube"},
				{"access_type", "offline"},
				{"prompt", "consent select_account"},
				{"code_challenge", challenge},
				{"code_challenge_method", "S256"},
				{"state", pendingState[i]}})
			// Kick's docs: a dummy redirect=127.0.0.1 before redirect_uri dodges a Next.js rewrite bug
			: form({{"client_id", id},
				{"response_type", "code"},
				{"redirect", "127.0.0.1"},
				{"redirect_uri", REDIRECT[KICK]},
				{"scope", "user:read channel:read channel:write streamkey:read"},
				{"code_challenge", challenge},
				{"code_challenge_method", "S256"},
				{"state", pendingState[i]}});
	QString base = i == YT ? "https://accounts.google.com/o/oauth2/v2/auth?" : "https://id.kick.com/oauth/authorize?";
	QDesktopServices::openUrl(QUrl::fromEncoded(base.toUtf8() + query));
	statusLabel->setText("Finish the login in your browser...");
}

QByteArray LiveDock::HandleAuth(const QByteArray &path)
{
	auto page = [](const QString &text) {
		return ("<html><body style='font:16px sans-serif;background:#1f1f23;color:#eee;padding:40px'>" +
			text.toHtmlEscaped() + "</body></html>")
			.toUtf8();
	};
	QUrl url = QUrl::fromEncoded("http://127.0.0.1" + path);
	QUrlQuery q(url);
	int i = url.path().endsWith("/yt") ? YT : KICK;
	if (pendingState[i].isEmpty() || q.queryItemValue("state") != pendingState[i])
		return page("Login link expired, start again from OBS.");
	pendingState[i].clear();
	QString code = q.queryItemValue("code", QUrl::FullyDecoded);
	if (code.isEmpty())
		return page("Login cancelled: " + q.queryItemValue("error", QUrl::FullyDecoded));

	Keys keys{{googleId, kickId}, {googleSecret, kickSecret}};
	QString verifier = pendingVerifier[i];
	Run([this, i, code, verifier, keys] {
		Account a;
		a.platform = i == YT ? "yt" : "kick";
		QString err = token_request(a, form({{"grant_type", "authorization_code"},
						     {"code", code},
						     {"client_id", keys.id[i]},
						     {"client_secret", keys.secret[i]},
						     {"redirect_uri", REDIRECT[i]},
						     {"code_verifier", verifier}}));
		QJsonObject j;
		long status = 0;
		if (err.isEmpty() && i == YT) {
			if ((status = api(a, "GET", YT_API "channels?part=snippet&mine=true", j)) != 200)
				err = api_error(status, j);
			auto item = j["items"].toArray().first().toObject();
			a.channel = item["id"].toString();
			a.name = item["snippet"].toObject()["title"].toString();
			if (err.isEmpty() && a.channel.isEmpty())
				err = "this Google account has no YouTube channel";
		} else if (err.isEmpty()) {
			if ((status = api(a, "GET", KICK_API "channels", j)) != 200)
				err = api_error(status, j);
			a.channel = a.name = j["data"].toArray().first().toObject()["slug"].toString();
		}
		QMetaObject::invokeMethod(
			this,
			[this, a, err] {
				if (!err.isEmpty()) {
					statusLabel->setText("Login failed: " + err);
					return;
				}
				Merge({a});
				FollowAccounts();
				statusLabel->setText("Linked " + a.name);
			},
			Qt::QueuedConnection);
	});
	return page("Logged in. You can close this tab and go back to OBS.");
}

// Adds new accounts, or replaces tokens/ids of ones we already have. Main thread only.
void LiveDock::Merge(const std::vector<Account> &updated)
{
	Save(); // keep titles typed in the UI
	for (auto &u : updated) {
		auto it = std::find_if(accounts.begin(), accounts.end(), [&](const Account &a) {
			return a.platform == u.platform && a.channel == u.channel;
		});
		if (it == accounts.end()) {
			accounts.push_back(u);
		} else {
			it->access = u.access;
			it->refresh = u.refresh;
			it->expires = u.expires;
			it->name = u.name;
			it->streamId = u.streamId;
			it->broadcastId = u.broadcastId;
			it->scheduled = u.scheduled;
		}
	}
	Save();
	Rebuild();
}

void LiveDock::Run(std::function<void()> work)
{
	if (worker.joinable())
		worker.join(); // buttons are disabled while busy, so this is the previous, finished job
	SetBusy(true, statusLabel->text());
	worker = std::thread([this, work] {
		work();
		QMetaObject::invokeMethod(this, [this] { SetBusy(false, statusLabel->text()); }, Qt::QueuedConnection);
	});
}

void LiveDock::SetBusy(bool busy, const QString &status)
{
	liveButton->setEnabled(!busy);
	titleButton->setEnabled(!busy);
	statusLabel->setText(status);
}

void LiveDock::GoLive()
{
	Save();
	bool adding = obs_frontend_streaming_active(); // already live: only start ticked accounts not streaming yet
	std::vector<Account> selected;
	for (auto &a : accounts)
		if (a.live && !(adding && liveIds.contains(a.platform + ":" + a.channel)))
			selected.push_back(a);
	if (selected.empty()) {
		statusLabel->setText(adding ? "All ticked accounts are already live." : "Tick at least one account.");
		return;
	}
	statusLabel->setText("Preparing streams...");
	Keys keys{{googleId, kickId}, {googleSecret, kickSecret}};
	Run([this, selected, keys, adding]() mutable {
		std::vector<std::pair<std::string, std::string>> ingest;
		QString err;
		for (auto &a : selected) {
			std::string server, key;
			err = fresh(a, keys);
			if (err.isEmpty())
				err = prepare_live(a, server, key);
			if (!err.isEmpty()) {
				err = a.name + ": " + err;
				break;
			}
			ingest.push_back({server, key});
		}
		QMetaObject::invokeMethod(
			this,
			[this, selected, ingest, err, adding] {
				Merge(selected);
				if (!err.isEmpty()) {
					statusLabel->setText("Go Live failed: " + err);
					return;
				}
				size_t first = 0;
				if (!adding) {
					// First account goes out on OBS's own stream output, the rest as Multistream outputs.
					obs_data_t *s = obs_data_create();
					obs_data_set_string(s, "server", ingest[0].first.c_str());
					obs_data_set_string(s, "key", ingest[0].second.c_str());
					obs_service_t *service = obs_service_create("rtmp_custom", "mosk_live_service", s, nullptr);
					obs_frontend_set_streaming_service(service);
					obs_service_release(service);
					obs_data_release(s);
					startedOutputs.clear();
					liveIds = {selected[0].platform + ":" + selected[0].channel};
					chatYt.clear();
					chatKick.clear();
					first = 1;
				}
				for (size_t i = first; i < selected.size(); i++) {
					std::string name = output_name(selected[i]).toStdString();
					multistream->UpsertOutput(name.c_str(), ingest[i].first.c_str(), ingest[i].second.c_str());
					startedOutputs.push_back(name);
					liveIds.insert(selected[i].platform + ":" + selected[i].channel);
					if (adding) // main is already up, so its encoders are ready
						multistream->SetOutputActive(name.c_str(), true);
				}

				for (auto &a : selected) {
					if (a.platform == "yt" && chatYt.isEmpty())
						chatYt = a.broadcastId;
					if (a.platform == "kick" && chatKick.isEmpty())
						chatKick = a.channel;
				}
				chat->SetChannels(chatYt, chatKick);

				if (adding) {
					statusLabel->setText(QString("Added %1 account(s) to the stream.").arg(selected.size()));
				} else {
					waitingForMain = true;
					obs_frontend_streaming_start();
					statusLabel->setText("Going live...");
				}
			},
			Qt::QueuedConnection);
	});
}

void LiveDock::UpdateTitles()
{
	Save();
	std::vector<Account> selected;
	for (auto &a : accounts)
		if (a.live && !a.title.isEmpty())
			selected.push_back(a);
	statusLabel->setText("Updating titles...");
	Keys keys{{googleId, kickId}, {googleSecret, kickSecret}};
	Run([this, selected, keys]() mutable {
		QStringList errors;
		for (auto &a : selected) {
			QString err = fresh(a, keys);
			if (err.isEmpty())
				err = set_title(a);
			if (!err.isEmpty())
				errors << a.name + ": " + err;
		}
		QMetaObject::invokeMethod(
			this,
			[this, selected, errors] {
				Merge(selected);
				statusLabel->setText(errors.isEmpty() ? "Titles updated." : errors.join("\n"));
			},
			Qt::QueuedConnection);
	});
}

void LiveDock::EndLive()
{
	waitingForMain = false;
	for (auto &name : startedOutputs)
		multistream->SetOutputActive(name.c_str(), false);
	startedOutputs.clear();
	liveIds.clear();
	if (obs_frontend_streaming_active())
		obs_frontend_streaming_stop(); // chat switches back on STREAMING_STOPPED
	else
		FollowAccounts();
	statusLabel->setText("Stream ended.");
}
