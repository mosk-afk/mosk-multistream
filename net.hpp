#pragma once

#include <atomic>
#include <string>
#include <vector>

// Blocking HTTP via libcurl, call from worker threads only. Returns the HTTP status (0 on network error).
// The transfer is aborted as soon as *running turns false.
long http_request(const std::string &method, const std::string &url, std::string &out,
		  const std::vector<std::string> &headers = {}, const std::string &body = "",
		  std::atomic<bool> *running = nullptr);
