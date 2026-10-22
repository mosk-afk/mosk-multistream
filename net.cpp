#include "net.hpp"
#include <curl/curl.h>

#define UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36"

static size_t write_cb(char *ptr, size_t size, size_t n, void *out)
{
	((std::string *)out)->append(ptr, size * n);
	return size * n;
}

static int abort_cb(void *running, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	return !running || ((std::atomic<bool> *)running)->load() ? 0 : 1;
}

long http_request(const std::string &method, const std::string &url, std::string &out,
		  const std::vector<std::string> &headers, const std::string &body, std::atomic<bool> *running)
{
	out.clear();
	CURL *c = curl_easy_init();
	if (!c)
		return 0;
	struct curl_slist *list = nullptr;
	for (auto &h : headers)
		list = curl_slist_append(list, h.c_str());
	if (method != "GET") {
		curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
		curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
	}
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
	curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, abort_cb);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, running);
	long code = 0;
	if (curl_easy_perform(c) == CURLE_OK)
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(list);
	curl_easy_cleanup(c);
	return code;
}
