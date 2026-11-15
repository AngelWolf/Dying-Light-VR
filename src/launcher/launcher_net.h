// HTTPS GET to a file (WinHTTP), for GitHub releases (spec section 10). Used ONLY when kRepoUrl is set - it is empty in
// this build, so nothing calls the network yet.
#pragma once

#include <filesystem>
#include <string>

namespace dlvr::launcher {

// false + *why on any failure (DNS, TLS, a non-200 status, a short read). Follows redirects (GitHub asset URLs redirect).
bool HttpGetToFile(const std::string& url, const std::filesystem::path& out, std::string* why);
bool HttpGetText(const std::string& url, std::string* body, std::string* why);

}  // namespace dlvr::launcher
