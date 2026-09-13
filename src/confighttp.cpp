/**
 * @file src/confighttp.cpp
 * @brief Definitions for the Web UI Config HTTP server.
 *
 * @todo Authentication, better handling of routes common to nvhttp, cleanup
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "process.h"

#include <array>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <random>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <cstdio>
#include <ctime>
#include <thread>
#include <utility>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include <boost/algorithm/string.hpp>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_https.hpp>
#include <Simple-Web-Server/utility.hpp>
#include <boost/asio/ssl/context_base.hpp>

#include "config.h"
#include "hdr_enhanced/api.h"
#include "hdr_enhanced/config.h"
#include "confighttp.h"
#include "clipboard_http.h"
#include "text_context/http.h"
#include "ai/credential_store.h"
#include "crypto.h"
#include "display_device/color_profile.h"
#include "display_device/session.h"
#include "file_mapping/file_mapping_store.h"
#include "file_handler.h"
#include "globals.h"
#include "hdr/client_display_capabilities.h"
#include "http_util.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "perf_recorder.h"
#include "platform/common.h"
#include "platform/run_command.h"
#include "rtsp.h"
#include "src/display_device/to_string.h"
#include "src/tray/tray_http.h"
#include "stream.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"
#include "version.h"
#include "webhook/webhook.h"
#include "webhook/webhook_api.h"

#ifdef _WIN32
  #include "ds5/config_api.h"
  #include <iphlpapi.h>
  #include "display_device/vdd_utils.h"
  #include "platform/windows/display_device/color_profile.h"
  #include "platform/windows/mic_write.h"
  #include "platform/windows/vulkan_hdr_bridge_session.h"
#endif

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp {
  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  // Prevent saveApp/deleteApp concurrent write to file_apps causing file corruption, non-blocking
  // return busy if not acquired
  static std::atomic<bool> apps_writing { false };
  static std::mutex file_mapping_store_transaction_mutex;

  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;

  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  enum class op_e {
    ADD,  ///< Add client
    REMOVE  ///< Remove client
  };

  void
  print_req(const req_https_t &request) {
    std::ostringstream log_stream;
    log_stream << "Request - TUNNEL: HTTPS"
               << ", METHOD: " << request->method
               << ", PATH: " << request->path;
    
    // Headers stay disabled because authentication and proxy headers may
    // contain credentials.
    /*
    // Headers
    if (!request->header.empty()) {
      log_stream << ", HEADERS: ";
      bool first = true;
      for (auto &[name, val] : request->header) {
        if (!first) log_stream << ", ";
        log_stream << name << "=" << (name == "Authorization" ? "CREDENTIALS REDACTED" : val);
        first = false;
      }
    }
    
    */

    static constexpr std::array safe_query_parameters {
      "bitrate"sv,
      "clientname"sv,
    };
    auto query_params = request->parse_query_string();
    http_util::append_allowed_request_log_fields(
      log_stream,
      ", PARAMS: "sv,
      query_params,
      safe_query_parameters,
      "&"sv
    );
    
    BOOST_LOG(verbose) << log_stream.str();
  }

  /**
   * @brief Send a response.
   * @param response The HTTP response object.
   * @param output_tree The JSON tree to send.
   */
  void send_response(resp_https_t response, const nlohmann::json &output_tree) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(output_tree.dump(), headers);
  }

  /**
   * @brief Validate the request content type and send bad request when mismatch.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param contentType The expected content type.
   * @return true if the request's Content-Type matches the expected type.
   *
   * Backport of upstream LizardByte/Sunshine 738ac93a0ec1 (CVE-2025-53095).
   * AlkaidLab does not have upstream's bad_request() helper, so the bad-request
   * response is inlined here.
   */
  bool check_content_type(resp_https_t response, req_https_t request, const std::string &contentType) {
    auto send_bad_request = [response](const std::string &error_message) {
      nlohmann::json tree;
      tree["status_code"] = 400;
      tree["status"] = false;
      tree["error"] = error_message;
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(SimpleWeb::StatusCode::client_error_bad_request, tree.dump(), headers);
    };
    auto requestContentType = request->header.find("content-type");
    if (requestContentType == request->header.end()) {
      send_bad_request("Content type not provided");
      return false;
    }
    if (!http_util::content_type_matches(requestContentType->second, contentType)) {
      send_bad_request("Content type mismatch");
      return false;
    }
    return true;
  }

  namespace {
    struct request_header_value_t {
      bool valid;
      std::optional<std::string_view> value;
    };

    request_header_value_t
    get_single_request_header(const req_https_t &request, const std::string_view name) {
      const auto range = request->header.equal_range(std::string { name });
      if (range.first == range.second) {
        return { true, std::nullopt };
      }

      auto first = range.first;
      const auto value = std::string_view { first->second };
      if (++first != range.second) {
        return { false, std::nullopt };
      }
      return { true, value };
    }

    void
    send_cross_site_forbidden(resp_https_t response) {
      const nlohmann::json body {
        { "status", false },
        { "error", "Cross-site request denied" },
      };
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      response->write(SimpleWeb::StatusCode::client_error_forbidden, body.dump(), headers);
    }

    bool
    authorize_browser_request(resp_https_t response, req_https_t request) {
      const auto path = std::string_view { request->path };
      if (!path.starts_with("/api/") &&
          !path.starts_with("/steam-api/") &&
          !path.starts_with("/steam-store/")) {
        return true;
      }

      const auto origin = get_single_request_header(request, "origin");
      const auto referer = get_single_request_header(request, "referer");
      const auto fetch_site = get_single_request_header(request, "sec-fetch-site");
      const auto host = get_single_request_header(request, "host");
      const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      if (!origin.valid || !referer.valid || !fetch_site.valid || !host.valid ||
          !http_util::browser_request_source_allowed(
            origin.value,
            referer.value,
            fetch_site.value,
            host.value.value_or(std::string_view {}),
            net::from_address(address) == net::PC
          )) {
        BOOST_LOG(debug) << "Web UI: rejected cross-site browser request to ["sv << request->path << ']';
        send_cross_site_forbidden(std::move(response));
        return false;
      }
      return true;
    }
  }  // namespace

  void
  send_unauthorized(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(error) << "Web UI: ["sv << address << "] -- not authorized"sv;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      { "WWW-Authenticate", R"(Basic realm="Sunshine Gamestream Host", charset="UTF-8")" }
    };
    response->write(SimpleWeb::StatusCode::client_error_unauthorized, headers);
  }

  /**
   * Logout endpoint: for localhost (PC) returns 200 so the browser does not show
   * a password dialog; for other allowed origins returns 401 with WWW-Authenticate
   * so the browser shows the credential prompt. Denied origins receive 403.
   */
  void
  handleLogout(resp_https_t response, req_https_t request) {
    if (!authorize_browser_request(response, request)) return;

    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto ip_type = net::from_address(address);

    if (ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- logout denied"sv;
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return;
    }

    if (ip_type == net::PC) {
      BOOST_LOG(info) << "Web UI: ["sv << address << "] -- logout (local), responding 200"sv;
      response->write(SimpleWeb::StatusCode::success_ok, "");
      return;
    }

    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- logout requested, responding 401"sv;
    send_unauthorized(response, request);
  }

  void
  send_redirect(resp_https_t response, req_https_t request, const char *path) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(error) << "Web UI: ["sv << address << "] -- not authorized, redirect"sv;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      { "Location", path }
    };
    response->write(SimpleWeb::StatusCode::redirection_temporary_redirect, headers);
  }

  bool
  authenticate(resp_https_t response, req_https_t request) {
    if (!authorize_browser_request(response, request)) return false;

    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto ip_type = net::from_address(address);

    if (ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- denied"sv;
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return false;
    }

    // If credentials are shown, redirect the user to a /welcome page
    if (config::sunshine.username.empty()) {
      send_redirect(response, request, "/welcome");
      return false;
    }

    if (ip_type == net::PC) {
      return true;
    }

    auto fg = util::fail_guard([&]() {
      send_unauthorized(response, request);
    });

    auto auth = request->header.find("authorization");
    if (auth == request->header.end()) {
      return false;
    }

    auto &rawAuth = auth->second;
    constexpr auto basicPrefix = "Basic "sv;
    if (rawAuth.length() <= basicPrefix.length() || 
        rawAuth.substr(0, basicPrefix.length()) != basicPrefix) {
      return false;
    }
    
    std::string authData;
    try {
      authData = SimpleWeb::Crypto::Base64::decode(rawAuth.substr(basicPrefix.length()));
    }
    catch (const std::exception &e) {
      BOOST_LOG(debug) << "Authentication: Base64 decode failed: " << e.what();
      return false;
    }

    int index = authData.find(':');
    if (index >= authData.size() - 1) {
      return false;
    }

    auto username = authData.substr(0, index);
    auto password = authData.substr(index + 1);
    auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();

    if (!boost::iequals(username, config::sunshine.username) || hash != config::sunshine.password) {
      return false;
    }

    fg.disable();
    return true;
  }

  void
  not_found(resp_https_t response, req_https_t request) {
    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());

    *response << "HTTP/1.1 404 NOT FOUND\r\n"
              << data.str();
  }

  void
  close_connection(resp_https_t response, req_https_t request) {
      *response << "HTTP/1.1 444 No Response\r\n";
      response->close_connection_after_response = true;
      return;
  }

  void
  getHtmlPage(resp_https_t response, req_https_t request, const std::string& pageName, bool requireAuth = true) {
    if (requireAuth && !authenticate(response, request)) return;

    print_req(request);

    std::string content = file_handler::read_file((std::string(WEB_DIR) + pageName).c_str());
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    if (pageName == "apps.html") {
      headers.emplace("Access-Control-Allow-Origin", "https://images.igdb.com/");
    }
    response->write(content, headers);
  }

  void
  getIndexPage(resp_https_t response, req_https_t request) {
    getHtmlPage(response, request, "index.html");
  }

  void
  getPinPage(resp_https_t response, req_https_t request) {
    getHtmlPage(response, request, "pin.html");
  }

  void
  getAppsPage(resp_https_t response, req_https_t request) {
    getHtmlPage(response, request, "apps.html");
  }

  void
  getClientsPage(resp_https_t response, req_https_t request) {
    getHtmlPage(response, request, "clients.html");
  }

  void
  getConfigPage(resp_https_t response, req_https_t request) {
    getHtmlPage(response, request, "config.html");
  }

  void
  getPasswordPage(resp_https_t response, req_https_t request) {
    getHtmlPage(response, request, "password.html");
  }

  void
  getWelcomePage(resp_https_t response, req_https_t request) {
    // 如果已经有用户名，要求认证后才能访问（防止未授权访问）
    // 认证通过后重定向到首页，认证失败则拒绝访问
    if (!config::sunshine.username.empty()) {
      if (!authenticate(response, request)) {
        return; // authenticate已经发送了401响应
      }
      // 认证通过，重定向到首页
      send_redirect(response, request, "/");
      return;
    }
    // 只有在没有用户名时才显示welcome页面（首次设置）
    getHtmlPage(response, request, "welcome.html", false);
  }

  void
  getTroubleshootingPage(resp_https_t response, req_https_t request) {
    getHtmlPage(response, request, "troubleshooting.html");
  }

  /**
   * 处理静态资源文件
   */
  void
  getStaticResource(resp_https_t response, req_https_t request, const std::string& path, const std::string& contentType) {
    // print_req(request);

    std::ifstream in(file_handler::path_from_utf8(path), std::ios::binary);
    if (!in.is_open()) {
      BOOST_LOG(error) << "Failed to open Web UI resource: " << path;
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      return;
    }
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", contentType);
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  void
  getFaviconImage(resp_https_t response, req_https_t request) {
    getStaticResource(response, request, WEB_DIR "images/sunshine.ico", "image/x-icon");
  }

  void
  getSunshineLogoImage(resp_https_t response, req_https_t request) {
    getStaticResource(response, request, WEB_DIR "images/logo-sunshine-256.png", "image/png");
  }

  void
  getAlkaidLabLogoImage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    getStaticResource(response, request, WEB_DIR "images/logo-alkaidlab.png", "image/png");
  }

  void
  getNatPierceLogoImage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    getStaticResource(response, request, WEB_DIR "images/logo-natpierce.png", "image/png");
  }

  /**
   * @brief 检查 child 是否是 parent 目录的子路径（防止路径穿越）
   */
  bool
  isChildPath(fs::path const &child, fs::path const &parent) {
    auto relPath = fs::relative(child, parent);
    return *(relPath.begin()) != fs::path("..");
  }

  void
  getBoxArt(resp_https_t response, req_https_t request) {
    print_req(request);

    // Extract image filename from request path
    std::string path = request->path;
    if (path.find("/boxart/") == 0) {
      path = path.substr(8); // Remove "/boxart/" prefix
    }
    path = SimpleWeb::Percent::decode(path);

    BOOST_LOG(debug) << "getBoxArt: Requested file: " << path;

    static const fs::path assetsRoot = fs::weakly_canonical(file_handler::path_from_utf8(SUNSHINE_ASSETS_DIR));
    static const fs::path coversRoot = fs::weakly_canonical(platf::appdata() / "covers");

    // First try to find in SUNSHINE_ASSETS_DIR
    const auto requestedPath = file_handler::path_from_utf8(path);
    fs::path targetPath = fs::weakly_canonical(assetsRoot / requestedPath);
    fs::path finalPath;
    bool found = false;

    // Strict check: Allow only files directly in assets root, no subdirectories
    if (targetPath.parent_path() == assetsRoot && fs::exists(targetPath) && fs::is_regular_file(targetPath)) {
      finalPath = targetPath;
      found = true;
      BOOST_LOG(debug) << "Found in boxart: " << file_handler::path_to_utf8(finalPath);
    }
    
    // If not found in boxart, try covers directory
    if (!found) {
      targetPath = fs::weakly_canonical(coversRoot / requestedPath);
      // For covers, we use isChildPath which allows subdirectories but prevents traversal out of root
      if (isChildPath(targetPath, coversRoot) && fs::exists(targetPath) && fs::is_regular_file(targetPath)) {
        finalPath = targetPath;
        found = true;
        BOOST_LOG(debug) << "Found in covers: " << file_handler::path_to_utf8(finalPath);
      }
    }

    if (!found) {
      // If still not found or invalid path, use default image
      BOOST_LOG(debug) << "Not found or invalid path, using default box.png";
      finalPath = assetsRoot / "box.png";
      // Ensure default file exists, otherwise we might fail later
      if (!fs::exists(finalPath)) {
        BOOST_LOG(warning) << "Default box.png not found at: " << file_handler::path_to_utf8(finalPath);
        response->write(SimpleWeb::StatusCode::client_error_not_found, "Image not found");
        return;
      }
    }

    const auto imagePath = file_handler::path_to_utf8(finalPath);

    // Get file size
    std::error_code ec;
    auto fileSize = fs::file_size(finalPath, ec);
    if (ec) {
      BOOST_LOG(warning) << "Failed to get file size for: " << imagePath;
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to read image file");
      return;
    }

    // Determine Content-Type from file extension
    std::string ext = finalPath.extension().string();
    if (!ext.empty() && ext[0] == '.') {
      ext = ext.substr(1);
    }
    
    auto mimeType = mime_types.find(ext);
    std::string contentType = "image/png"; // Default type
    
    if (mimeType != mime_types.end()) {
      contentType = mimeType->second;
    }
    
    BOOST_LOG(debug) << "Serving boxart: " << imagePath << " (Content-Type: " << contentType << ", Size: " << fileSize << " bytes)";

    // Return image resource
    std::ifstream in(finalPath, std::ios::binary);
    if (!in.is_open()) {
      BOOST_LOG(warning) << "Failed to open image file: " << imagePath;
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to open image file");
      return;
    }

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", contentType);
    headers.emplace("Content-Length", std::to_string(fileSize));
    headers.emplace("Cache-Control", "max-age=3600"); // Add caching to reduce load
    
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  void
  getNodeModules(resp_https_t response, req_https_t request) {
    // print_req(request);
    fs::path webDirPath = file_handler::path_from_utf8(WEB_DIR);
    fs::path nodeModulesPath(webDirPath / "assets");

    // .relative_path is needed to shed any leading slash that might exist in the request path
    const auto decodedRequestPath = SimpleWeb::Percent::decode(request->path);
    auto filePath = fs::weakly_canonical(webDirPath / file_handler::path_from_utf8(decodedRequestPath).relative_path());

    // Don't do anything if file does not exist or is outside the assets directory
    if (!isChildPath(filePath, nodeModulesPath)) {
      BOOST_LOG(warning) << "Someone requested a path " << filePath << " that is outside the assets folder";
      response->write(SimpleWeb::StatusCode::client_error_bad_request, "Bad Request");
    }
    else if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) {
      response->write(SimpleWeb::StatusCode::client_error_not_found);
    }
    else {
      auto relPath = fs::relative(filePath, webDirPath);
      // get the mime type from the file extension mime_types map
      // remove the leading period from the extension
      auto mimeType = mime_types.find(relPath.extension().string().substr(1));
      // check if the extension is in the map at the x position
      if (mimeType != mime_types.end()) {
        // if it is, set the content type to the mime type
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", mimeType->second);
        std::ifstream in(filePath, std::ios::binary);
        if (!in.is_open()) {
          BOOST_LOG(error) << "Failed to open Web UI asset: " << file_handler::path_to_utf8(filePath);
          response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
          return;
        }
        response->write(SimpleWeb::StatusCode::success_ok, in, headers);
      }
      // do not return any file if the type is not in the map
    }
  }

  void
  getApps(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    std::string content = file_handler::read_file(config::stream.file_apps.c_str());
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(content, headers);
  }

  /**
   * @brief Snapshot of log cache state, swapped atomically via shared_ptr.
   */
  struct LogCacheSnapshot {
    std::shared_ptr<const std::string> content;  ///< Cached log content (nullptr in offset-only mode)
    std::uintmax_t file_size = 0;                ///< Actual file size on disk when snapshot was taken
    std::intmax_t mtime_ns = 0;                  ///< File mtime when snapshot was taken
    std::uintmax_t start_offset = 0;             ///< File byte position where content begins
  };

  /**
   * @brief Try to read only the new tail of the log file and append to existing content.
   * @return New content on success, nullptr on any failure (caller should fall back to full read).
   */
  static std::shared_ptr<const std::string> try_incremental_log_read(
    const std::filesystem::path &log_path,
    std::uintmax_t prev_size,
    std::uintmax_t current_size,
    const std::shared_ptr<const std::string> &old_content) {
    if (current_size <= prev_size || prev_size == 0 || !old_content) {
      return nullptr;
    }
    std::ifstream in(log_path, std::ios::binary);
    if (!in || !in.seekg(static_cast<std::streamoff>(prev_size))) {
      return nullptr;
    }
    const auto tail_len = current_size - prev_size;
    std::string tail(tail_len, '\0');
    if (!in.read(tail.data(), static_cast<std::streamsize>(tail_len))) {
      return nullptr;
    }
    return std::make_shared<const std::string>(*old_content + tail);
  }

  /**
   * @brief Read a specific byte range [offset, offset+length) from a file.
   * @return Content string on success, nullptr on failure.
   */
  static std::shared_ptr<const std::string>
  read_file_range(const std::filesystem::path &path, std::uintmax_t offset, std::uintmax_t length) {
    std::ifstream in(path, std::ios::binary);
    if (!in || !in.seekg(static_cast<std::streamoff>(offset))) {
      return nullptr;
    }
    std::string content(static_cast<std::size_t>(length), '\0');
    if (!in.read(content.data(), static_cast<std::streamsize>(length))) {
      return nullptr;
    }
    return std::make_shared<const std::string>(std::move(content));
  }

  /**
   * @brief Get the logs from the log file.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * Dual mode via X-Log-Offset header:
   *   - Without header: stream full log file from disk (for download)
   *   - With header:    cached or offset-based incremental support (for live viewer)
   *
   * When the log file is small (≤ 4 MB), the entire file is cached in memory for fast serving.
   * When the log file exceeds 4 MB, no content is cached; reads go directly to disk at the
   * client's offset (offset-only mode), avoiding unbounded memory growth.
   *
   * @api_examples{/api/logs| GET| null}
   */
  void
  getLogs(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    //print_req(request);

    const auto log_path = file_handler::path_from_utf8(config::sunshine.log_file);

    // --- Mode 1: No X-Log-Offset header → stream full file from disk (download) ---
    auto offset_it = request->header.find("X-Log-Offset");
    if (offset_it == request->header.end()) {
      std::ifstream in(log_path, std::ios::binary);
      if (!in.is_open()) {
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to open log file");
        return;
      }
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "text/plain; charset=utf-8");
      headers.emplace("Content-Disposition", "attachment; filename=\"sunshine.log\"");
      // Omit Content-Length: log file is actively written (TOCTOU risk)
      response->write(SimpleWeb::StatusCode::success_ok, in, headers);
      return;
    }

    // --- Mode 2: X-Log-Offset present → cached or offset-only mode for live viewer ---

    // When file ≤ MAX_LOG_CACHE_SIZE: cached in memory.  Otherwise: disk reads only.
    static constexpr std::uintmax_t MAX_LOG_CACHE_SIZE = 4 * 1024 * 1024;   // 4 MB
    static constexpr std::uintmax_t MAX_RESPONSE_SIZE  = 4 * 1024 * 1024;   // 4 MB

    static std::atomic<std::shared_ptr<const LogCacheSnapshot>> log_cache;

    // Check file status
    std::error_code ec;
    auto current_size = std::filesystem::file_size(log_path, ec);
    if (ec) {
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to read log file");
      return;
    }
    auto current_mtime = std::filesystem::last_write_time(log_path, ec);
    if (ec) {
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to read log file");
      return;
    }
    auto current_mtime_ns = current_mtime.time_since_epoch().count();

    // Refresh cache if file changed
    auto snapshot = log_cache.load();
    const bool cache_stale = !snapshot || current_size != snapshot->file_size || current_mtime_ns != snapshot->mtime_ns;
    if (cache_stale) {
      auto new_snap = std::make_shared<LogCacheSnapshot>();
      new_snap->file_size = current_size;
      new_snap->mtime_ns = current_mtime_ns;

      if (current_size <= MAX_LOG_CACHE_SIZE) {
        // --- Cached mode: file fits in memory ---
        std::shared_ptr<const std::string> new_content;
        if (snapshot && snapshot->content && snapshot->file_size > 0 && current_size > snapshot->file_size) {
          new_content = try_incremental_log_read(log_path, snapshot->file_size, current_size, snapshot->content);
        }
        if (!new_content) {
          // Use sampled current_size to avoid TOCTOU unsigned underflow at start_offset
          auto read_len = std::min(current_size, MAX_LOG_CACHE_SIZE);
          auto read_start = current_size - read_len;
          new_content = read_file_range(log_path, read_start, read_len);
        }
        if (!new_content) {
          if (current_size > 0) {
            response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to read log file");
            return;
          }
          new_content = std::make_shared<const std::string>();
        }
        new_snap->content = std::move(new_content);
        new_snap->start_offset = current_size - new_snap->content->size();
      }
      else {
        // --- Offset-only mode: file too large, don't cache content ---
        new_snap->content = nullptr;
        new_snap->start_offset = 0;
      }

      // CAS publish: avoid overwriting a newer snapshot from a concurrent thread
      if (!log_cache.compare_exchange_strong(snapshot, new_snap)) {
        // CAS failed: snapshot already updated by compare_exchange_strong
      }
      else {
        snapshot = std::move(new_snap);
      }
    }

    // Parse client offset
    std::uintmax_t client_offset = 0;
    try {
      std::string offset_str(offset_it->second);
      boost::algorithm::trim(offset_str);
      if (!offset_str.empty()) {
        client_offset = std::stoull(offset_str);
      }
    }
    catch (const std::invalid_argument &) {
      client_offset = 0;
    }
    catch (const std::out_of_range &) {
      client_offset = 0;
    }

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/plain");
    headers.emplace("X-Log-Size", std::to_string(snapshot->file_size));
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    // No change in logs → 304
    if (client_offset > 0 && client_offset == snapshot->file_size) {
      headers.emplace("X-Log-Range", "unchanged");
      response->write(SimpleWeb::StatusCode::redirection_not_modified, headers);
      return;
    }

    if (snapshot->content) {
      // === Cached mode: serve from memory ===
      if (client_offset > 0 && client_offset >= snapshot->start_offset && client_offset < snapshot->file_size) {
        auto cache_pos = client_offset - snapshot->start_offset;
        headers.emplace("X-Log-Range", "incremental");
        auto delta = snapshot->content->substr(static_cast<std::size_t>(cache_pos));
        response->write(SimpleWeb::StatusCode::success_ok, delta, headers);
      }
      else {
        headers.emplace("X-Log-Range", "full");
        response->write(SimpleWeb::StatusCode::success_ok, *snapshot->content, headers);
      }
    }
    else {
      // === Offset-only mode: read from disk, bounded by snapshot->file_size ===
      if (client_offset > 0 && client_offset < snapshot->file_size) {
        auto delta_size = snapshot->file_size - client_offset;
        if (delta_size <= MAX_RESPONSE_SIZE) {
          auto data = read_file_range(log_path, client_offset, delta_size);
          if (data) {
            headers.emplace("X-Log-Range", "incremental");
            response->write(SimpleWeb::StatusCode::success_ok, *data, headers);
            return;
          }
        }
      }
      // Delta too large, read failed, or first request: return tail up to snapshot->file_size
      auto tail_len = std::min(snapshot->file_size, MAX_RESPONSE_SIZE);
      auto tail_start = snapshot->file_size - tail_len;
      auto tail = read_file_range(log_path, tail_start, tail_len);
      if (!tail) {
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to read log file");
        return;
      }
      headers.emplace("X-Log-Range", "full");
      response->write(SimpleWeb::StatusCode::success_ok, *tail, headers);
    }
  }

  void
  saveApp(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;

    print_req(request);

    // Prevent concurrent write to file_apps causing file corruption
    bool expected = false;
    if (!apps_writing.compare_exchange_strong(expected, true)) {
      pt::ptree outputTree;
      outputTree.put("status", "false");
      outputTree.put("error", "Another save operation is in progress");
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(SimpleWeb::StatusCode::client_error_conflict, data.str());
      return;
    }
    auto writing_guard = util::fail_guard([]() { apps_writing = false; });

    std::stringstream ss;
    ss << request->content.rdbuf();

    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    pt::ptree inputTree, fileTree;

    try {
      pt::read_json(ss, inputTree);
      file_handler::read_json(config::stream.file_apps, fileTree);

      auto &apps_node = fileTree.get_child("apps"s);
      auto &input_apps_node = inputTree.get_child("apps"s);
      auto &input_edit_node = inputTree.get_child("editApp"s);

      const auto normalize_gamepad = [&](pt::ptree &app_node) {
        auto gamepad = app_node.get_optional<std::string>("gamepad");
        if (gamepad && gamepad->empty()) {
          app_node.erase("gamepad");
        }
        else if (gamepad && *gamepad != "auto"sv && *gamepad != "x360"sv && *gamepad != "ds4"sv && *gamepad != "ds5"sv) {
          outputTree.put("status", "false");
          outputTree.put("error", "Invalid per-app gamepad type");
          return false;
        }

        return true;
      };

      for (auto &[_, app_node] : input_apps_node) {
        if (!normalize_gamepad(app_node)) {
          return;
        }
      }

      // Validate app name when editing a specific app
      if (!input_edit_node.empty()) {
        auto app_name = input_edit_node.get<std::string>("name", "");
        if (app_name.empty() || app_name.size() > 256) {
          outputTree.put("status", "false");
          outputTree.put("error", "App name must be 1-256 characters");
          return;
        }
      }

      if (input_edit_node.empty()) {
        fileTree.erase("apps");
        fileTree.push_back(std::make_pair("apps", input_apps_node));
      }
      else {
        auto prep_cmd = input_edit_node.get_child_optional("prep-cmd");
        if (prep_cmd && prep_cmd->empty()) {
          input_edit_node.erase("prep-cmd");
        }

        auto detached = input_edit_node.get_child_optional("detached");
        if (detached && detached->empty()) {
          input_edit_node.erase("detached");
        }

        if (!normalize_gamepad(input_edit_node)) {
          return;
        }

        int index = input_edit_node.get<int>("index");
        input_edit_node.erase("index");

        if (index == -1) {
          apps_node.push_back(std::make_pair("", input_edit_node));
        }
        else {
          // Unfortunately Boost PT does not allow to directly edit the array, copy should do the trick
          pt::ptree newApps;
          int i = 0;
          for (auto &[_, app_node] : input_apps_node) {
            newApps.push_back(std::make_pair("", i == index ? input_edit_node : app_node));
            i++;
          }
          fileTree.erase("apps");
          fileTree.push_back(std::make_pair("apps", newApps));
        }
      }

      file_handler::write_json(config::stream.file_apps, fileTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveApp: "sv << e.what();

      outputTree.put("status", "false");
      outputTree.put("error", "Invalid Input JSON");
      return;
    }

    BOOST_LOG(info) << "SaveApp: configuration saved successfully"sv;
    outputTree.put("status", "true");
    proc::refresh(config::stream.file_apps);
  }

  void
  deleteApp(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    // Prevent concurrent write to file_apps causing file corruption
    bool expected = false;
    if (!apps_writing.compare_exchange_strong(expected, true)) {
      pt::ptree outputTree;
      outputTree.put("status", "false");
      outputTree.put("error", "Another operation is in progress");
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(SimpleWeb::StatusCode::client_error_conflict, data.str());
      return;
    }
    auto writing_guard = util::fail_guard([]() { apps_writing = false; });

    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_json(data, outputTree);
      response->write(data.str());
    });
    pt::ptree fileTree;
    try {
      file_handler::read_json(config::stream.file_apps, fileTree);
      auto &apps_node = fileTree.get_child("apps"s);
      int index = stoi(request->path_match[1]);

      int apps_count = static_cast<int>(apps_node.size());
      if (index < 0 || index >= apps_count) {
        outputTree.put("status", "false");
        outputTree.put("error", "Invalid Index");
        return;
      }
      else {
        // Unfortunately Boost PT does not allow to directly edit the array, copy should do the trick
        pt::ptree newApps;
        int i = 0;
        for (const auto &kv : apps_node) {
          if (i++ != index) {
            newApps.push_back(std::make_pair("", kv.second));
          }
        }
        fileTree.erase("apps");
        fileTree.push_back(std::make_pair("apps", newApps));
      }
      file_handler::write_json(config::stream.file_apps, fileTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "DeleteApp: "sv << e.what();
      outputTree.put("status", "false");
      outputTree.put("error", "Invalid File JSON");
      return;
    }

    BOOST_LOG(info) << "DeleteApp: configuration deleted successfully"sv;
    outputTree.put("status", "true");
    proc::refresh(config::stream.file_apps);
  }

  /**
   * @brief Delete multiple apps in a single atomic operation.
   *
   * Expects JSON body: {"indices":[<int>, <int>, ...]}
   *
   * Rationale: clients that want to delete N apps cannot just loop on
   * DELETE /api/apps/{id} — each successful delete shifts the remaining
   * indices, and a second concurrent caller is rejected by apps_writing.
   * By taking the lock once and rebuilding the array under it, we avoid both
   * problems: indices are interpreted against a single snapshot, the JSON file
   * is rewritten once, and proc::refresh runs only once.
   *
   * Response:
   *   200 {"status":"true",  "deleted":<N>, "remaining":<M>}
   *   400 {"status":"false", "error":"..."} for content-type / JSON / index /
   *        file-write business errors. Follows uploadCover() convention in
   *        this same file (status code derived from presence of 'error' key
   *        inside the fail_guard).
   *   401  not authenticated (via authenticate)
   *   409  another apps-writer is already in flight
   *
   * @api_examples{/api/apps/batch-delete| POST| {"indices":[2,5,7]}}
   */
  void
  batchDeleteApps(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;

    print_req(request);

    // Same single-writer guard as deleteApp / saveApp.
    bool expected = false;
    if (!apps_writing.compare_exchange_strong(expected, true)) {
      pt::ptree busy;
      busy.put("status", "false");
      busy.put("error", "Another operation is in progress");
      std::ostringstream data;
      pt::write_json(data, busy);
      response->write(SimpleWeb::StatusCode::client_error_conflict, data.str());
      return;
    }
    auto writing_guard = util::fail_guard([]() { apps_writing = false; });

    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      // Mirror uploadCover's convention in this file: when fail_guard fires
      // with an 'error' field, emit 4xx so clients can distinguish business
      // failures from success rather than parsing status:"false" out of a 200.
      SimpleWeb::StatusCode code = SimpleWeb::StatusCode::success_ok;
      if (outputTree.get_child_optional("error").has_value()) {
        code = SimpleWeb::StatusCode::client_error_bad_request;
      }
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(code, data.str());
    });

    std::stringstream ss;
    ss << request->content.rdbuf();

    std::set<int> indices_to_remove;
    try {
      pt::ptree body;
      pt::read_json(ss, body);

      auto indices_node = body.get_child_optional("indices");
      if (!indices_node) {
        outputTree.put("status", "false");
        outputTree.put("error", "Missing 'indices' array");
        return;
      }

      if (indices_node->size() > 1024) {
        outputTree.put("status", "false");
        outputTree.put("error", "Too many indices in a single request");
        return;
      }
      for (const auto &kv : *indices_node) {
        // boost::property_tree json reader stores array values as anonymous
        // children with empty keys. get_value<int>() will throw if the value
        // is not parseable as an integer.
        int idx = kv.second.get_value<int>();
        indices_to_remove.insert(idx);
      }
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "BatchDeleteApps: invalid JSON body: "sv << e.what();
      outputTree.put("status", "false");
      outputTree.put("error", "Invalid JSON body");
      return;
    }

    pt::ptree fileTree;
    try {
      file_handler::read_json(config::stream.file_apps, fileTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "BatchDeleteApps: "sv << e.what();
      outputTree.put("status", "false");
      outputTree.put("error", "Invalid File JSON");
      return;
    }

    // Validate every index against the single snapshot we just loaded BEFORE
    // any write, so a partially invalid request fails atomically.
    const int apps_count = static_cast<int>(fileTree.get_child("apps"s).size());
    for (int idx : indices_to_remove) {
      if (idx < 0 || idx >= apps_count) {
        outputTree.put("status", "false");
        outputTree.put("error", "Invalid Index");
        return;
      }
    }

    // Empty selection: success no-op. Skip the write+refresh, but still emit
    // 'remaining' so the success contract matches the non-empty path.
    if (indices_to_remove.empty()) {
      outputTree.put("status", "true");
      outputTree.put("deleted", 0);
      outputTree.put("remaining", apps_count);
      return;
    }

    int deleted_count = 0;
    int remaining_count = 0;
    try {
      auto &apps_node = fileTree.get_child("apps"s);
      pt::ptree newApps;
      int i = 0;
      for (const auto &kv : apps_node) {
        if (indices_to_remove.find(i) == indices_to_remove.end()) {
          newApps.push_back(std::make_pair("", kv.second));
        }
        else {
          ++deleted_count;
        }
        ++i;
      }
      remaining_count = static_cast<int>(newApps.size());
      fileTree.erase("apps");
      fileTree.push_back(std::make_pair("apps", newApps));

      file_handler::write_json(config::stream.file_apps, fileTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "BatchDeleteApps: "sv << e.what();
      outputTree.put("status", "false");
      outputTree.put("error", "Invalid File JSON");
      return;
    }

    BOOST_LOG(info) << "BatchDeleteApps: removed "sv << deleted_count
                    << " app(s), "sv << remaining_count << " remaining"sv;
    outputTree.put("status", "true");
    outputTree.put("deleted", deleted_count);
    outputTree.put("remaining", remaining_count);
    proc::refresh(config::stream.file_apps);
  }

  void
  uploadCover(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;

    std::stringstream ss;
    std::stringstream configStream;
    ss << request->content.rdbuf();
    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      SimpleWeb::StatusCode code = SimpleWeb::StatusCode::success_ok;
      if (outputTree.get_child_optional("error").has_value()) {
        code = SimpleWeb::StatusCode::client_error_bad_request;
      }

      pt::write_json(data, outputTree);
      response->write(code, data.str());
    });
    pt::ptree inputTree;
    try {
      pt::read_json(ss, inputTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "UploadCover: "sv << e.what();
      outputTree.put("status", "false");
      outputTree.put("error", e.what());
      return;
    }

    auto key = inputTree.get("key", "");
    if (key.empty()) {
      outputTree.put("error", "Cover key is required");
      return;
    }
    auto url = inputTree.get("url", "");

    const std::string coverdir = file_handler::path_to_utf8(platf::appdata() / "covers");
    file_handler::make_directory(coverdir);

    const auto cover_path = file_handler::path_from_utf8(coverdir) / (http::url_escape(key) + ".png");
    const std::string path = file_handler::path_to_utf8(cover_path);
    if (!url.empty()) {
      if (!http::download_public_cover_image(url, path)) {
        outputTree.put("error", "Failed to download public HTTPS cover");
        return;
      }
    }
    else {
      // Limit base64 data size to prevent memory exhaustion
      // (10MB decoded to about 7.5MB, enough for cover images)
      constexpr std::size_t MAX_COVER_BASE64_SIZE = 10 * 1024 * 1024;
      auto base64_str = inputTree.get<std::string>("data");
      if (base64_str.size() > MAX_COVER_BASE64_SIZE) {
        outputTree.put("error", "Cover image too large (max 10MB)");
        return;
      }
      auto data = SimpleWeb::Crypto::Base64::decode(base64_str);

      std::ofstream imgfile(file_handler::path_from_utf8(path), std::ios::binary);
      if (!imgfile.is_open()) {
        outputTree.put("error", "Failed to create file");
        return;
      }
      imgfile.write(data.data(), (int) data.size());
      imgfile.close();
      
      if (imgfile.fail()) {
        outputTree.put("error", "Failed to write file");
        return;
      }
    }
    outputTree.put("path", path);
  }

  void
  getConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    auto devices { display_device::enum_available_devices() };
    pt::ptree devices_nodes;
    for (const auto &[device_id, data] : devices) {
      pt::ptree devices_node;
      devices_node.put("device_id"s, device_id);
      devices_node.put("data"s, to_string(data));
      devices_nodes.push_back(std::make_pair(""s, devices_node));
    }

    auto adapters { platf::adapter_names() };
    pt::ptree adapters_nodes;
    for (const auto &adapter_name : adapters) {
      pt::ptree adapters_node;
      adapters_node.put("name"s, adapter_name);
      adapters_nodes.push_back(std::make_pair(""s, adapters_node));
    }

    outputTree.add_child("display_devices", devices_nodes);
    outputTree.add_child("adapters", adapters_nodes);
    outputTree.put("status", "true");
    outputTree.put("platform", SUNSHINE_PLATFORM);
    outputTree.put("version", PROJECT_VER);

    auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
    for (auto &[name, value] : vars) {
      outputTree.put(std::move(name), std::move(value));
    }

    outputTree.put("active_encoder", video::active_encoder_name());
    // Configuration capability only; never expose the paired-client tunnel token here.
    outputTree.put("usb_forwarding_config_version", "1");
    outputTree.put("pair_name", nvhttp::get_pair_name());
  }

  void
  getLocale(resp_https_t response, req_https_t request) {
    // we need to return the locale whether authenticated or not

    print_req(request);

    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    outputTree.put("status", "true");
    outputTree.put("locale", config::sunshine.locale);
  }

  std::vector<std::string>
  split(const std::string &str, char delimiter) {
    std::vector<std::string> tokens;
    size_t start = 0, end = 0;
    while ((end = str.find(delimiter, start)) != std::string::npos) {
      tokens.push_back(str.substr(start, end - start));
      start = end + 1;
    }
    tokens.push_back(str.substr(start));
    return tokens;
  }

  bool
  saveVddSettings(std::string resArray, std::string fpsArray, std::string gpu_name) {
    pt::ptree iddOptionTree;
    pt::ptree global_node;
    pt::ptree resolutions_nodes;

    // prepare resolutions setting for vdd
    boost::regex pattern("\\[|\\]|\\s+");
    char delimiter = ',';

    // 添加全局刷新率到global节点
    for (const auto &fps : split(boost::regex_replace(fpsArray, pattern, ""), delimiter)) {
      global_node.add("g_refresh_rate", fps);
    }

    std::string str = boost::regex_replace(resArray, pattern, "");
    boost::algorithm::trim(str);
    for (const auto &resolution : split(str, delimiter)) {
      auto index = resolution.find('x');
      if(index == std::string::npos) {
        return false;
      }
      pt::ptree res_node;
      res_node.put("width", resolution.substr(0, index));
      res_node.put("height", resolution.substr(index + 1));
      resolutions_nodes.push_back(std::make_pair("resolution"s, res_node));
    }

    // 类似于 config.cpp 中的 path_f 函数逻辑，使用相对路径
    std::filesystem::path idd_option_path = platf::appdata() / "vdd_settings.xml";

    BOOST_LOG(info) << "VDD配置文件路径: " << file_handler::path_to_utf8(idd_option_path);

    if (!fs::exists(idd_option_path)) {
        return false;
    }

    // 先读取现有配置文件
    pt::ptree existing_root;
    pt::ptree root;

    try {
      {
        std::ifstream input(idd_option_path, std::ios::binary);
        if (!input.is_open()) {
          throw std::runtime_error("Unable to open VDD configuration file");
        }
        pt::read_xml(input, existing_root);
      }
      // 如果现有配置文件中已有vdd_settings节点
      if (existing_root.get_child_optional("vdd_settings")) {
        // 复制现有配置
        iddOptionTree = existing_root.get_child("vdd_settings");

        // 更新需要更改的部分
        pt::ptree monitor_node;
        monitor_node.put("count", 1);

        pt::ptree gpu_node;
        gpu_node.put("friendlyname", gpu_name.empty() ? "default" : gpu_name);

        // 替换配置
        iddOptionTree.put_child("monitors", monitor_node);
        iddOptionTree.put_child("gpu", gpu_node);
        iddOptionTree.put_child("global", global_node);
        iddOptionTree.put_child("resolutions", resolutions_nodes);
      } else {
        // 如果没有vdd_settings节点，创建新的
        pt::ptree monitor_node;
        monitor_node.put("count", 1);

        pt::ptree gpu_node;
        gpu_node.put("friendlyname", gpu_name.empty() ? "default" : gpu_name);

        iddOptionTree.add_child("monitors", monitor_node);
        iddOptionTree.add_child("gpu", gpu_node);
        iddOptionTree.add_child("global", global_node);
        iddOptionTree.add_child("resolutions", resolutions_nodes);
      }
    } catch(std::exception &e) {
      // 读取失败，创建新的配置
      BOOST_LOG(warning) << "读取现有VDD配置失败，创建新配置: " << e.what();

      pt::ptree monitor_node;
      monitor_node.put("count", 1);

      pt::ptree gpu_node;
      gpu_node.put("friendlyname", gpu_name.empty() ? "default" : gpu_name);

      iddOptionTree.add_child("monitors", monitor_node);
      iddOptionTree.add_child("gpu", gpu_node);
      iddOptionTree.add_child("global", global_node);
      iddOptionTree.add_child("resolutions", resolutions_nodes);
    }

    root.add_child("vdd_settings", iddOptionTree);
    try {
      // 使用更紧凑的XML格式设置，减少不必要的空白
      auto setting = boost::property_tree::xml_writer_make_settings<std::string>(' ', 2);
      std::ostringstream oss;
      write_xml(oss, root, setting);

      // 清理多余空行，保持XML格式整洁
      std::string xml_content = oss.str();
      boost::regex empty_lines_regex("\\n\\s*\\n");
      xml_content = boost::regex_replace(xml_content, empty_lines_regex, "\n");

      std::ofstream file(idd_option_path);
      if (!file.is_open()) {
        throw std::runtime_error("Unable to open VDD configuration file");
      }
      file << xml_content;
      file.flush();
      if (!file) {
        throw std::runtime_error("Unable to write VDD configuration file");
      }

      return true;
    }
    catch(std::exception &e) {
      BOOST_LOG(warning) << "写入VDD配置失败: " << e.what();
      return false;
    }
  }

  void
  write_runtime_error(resp_https_t response, SimpleWeb::StatusCode http_status, int status_code, const std::string &status_message);

  bool
  require_localhost(resp_https_t response, req_https_t request, const std::string &action);


  void
  saveConfig(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    pt::ptree inputTree;

    try {
      pt::read_json(ss, inputTree);
      std::string resArray = inputTree.get<std::string>("resolutions", "[]");
      std::string fpsArray = inputTree.get<std::string>("fps", "[]");
      std::string gpu_name = inputTree.get<std::string>("adapter_name", "");

      // Validate config field lengths to prevent abuse
      auto sunshine_name = inputTree.get<std::string>("sunshine_name", "");
      if (sunshine_name.size() > 256) {
        outputTree.put("status", "false");
        outputTree.put("error", "sunshine_name too long (max 256)");
        return;
      }
      if (gpu_name.size() > 256) {
        outputTree.put("status", "false");
        outputTree.put("error", "adapter_name too long (max 256)");
        return;
      }

      saveVddSettings(resArray, fpsArray, gpu_name);

      // 将 inputTree 转换为 std::map（保证有序）
      std::map<std::string, std::string> fullConfig;
      for (const auto &kv : inputTree) {
        if (kv.first == "usb_forwarding_config_version") continue;
        std::string value = inputTree.get<std::string>(kv.first);
        fullConfig[kv.first] = value;
      }

      // 更新配置
      if (!config::update_full_config(fullConfig)) {
        outputTree.put("status", "false");
        outputTree.put("error", "failed to persist configuration");
        return;
      }
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveConfig: "sv << e.what();
      outputTree.put("status", "false");
      outputTree.put("error", e.what());
      return;
    }

    outputTree.put("status", "true");
  }

  void
  getGamepadConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    pt::ptree outputTree;
    auto response_guard = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    const auto config_snapshot = config::get_config_snapshot();
    if (!config_snapshot) {
      outputTree.put("status", "false");
      outputTree.put("error", "failed to read controller configuration");
      return;
    }

    const auto get_value = [&](std::string_view key, std::string_view fallback) {
      const auto entry = config_snapshot->find(std::string {key});
      return entry == config_snapshot->end() ? std::string {fallback} : entry->second;
    };
    outputTree.put("status", "true");
    outputTree.put("gamepad", get_value("gamepad", "auto"));
    outputTree.put("motion_as_ds4", get_value("motion_as_ds4", "true"));
    outputTree.put("touchpad_as_ds4", get_value("touchpad_as_ds4", "true"));
    outputTree.put("ds4_back_as_touchpad_click", get_value("ds4_back_as_touchpad_click", "true"));
    outputTree.put("enable_dsu_server", get_value("enable_dsu_server", "false"));
    outputTree.put("dsu_server_port", get_value("dsu_server_port", "26760"));
  }

  void
  saveGamepadConfig(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;

    pt::ptree outputTree;
    auto response_guard = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    try {
      pt::ptree inputTree;
      std::stringstream body;
      body << request->content.rdbuf();
      pt::read_json(body, inputTree);
      if (inputTree.empty() || inputTree.size() > 6) {
        throw std::invalid_argument("controller configuration patch must contain 1 to 6 fields");
      }

      const std::set<std::string> boolean_fields {
        "ds4_back_as_touchpad_click",
        "enable_dsu_server",
        "motion_as_ds4",
        "touchpad_as_ds4",
      };
      std::map<std::string, std::string> updates;
      for (const auto &[key, node] : inputTree) {
        if (!node.empty()) {
          throw std::invalid_argument("controller configuration fields must be scalar values");
        }
        const auto value = node.get_value<std::string>();
        if (key == "gamepad") {
          if (value != "auto"sv && value != "x360"sv && value != "ds4"sv && value != "ds5"sv) {
            throw std::invalid_argument("invalid gamepad mode");
          }
        }
        else if (boolean_fields.contains(key)) {
          if (value != "true"sv && value != "false"sv) {
            throw std::invalid_argument("controller boolean fields must be true or false");
          }
        }
        else if (key == "dsu_server_port") {
          std::size_t parsed = 0;
          const auto port = std::stoi(value, &parsed);
          if (parsed != value.size() || port < 1024 || port > 65535) {
            throw std::invalid_argument("DSU port must be between 1024 and 65535");
          }
        }
        else {
          throw std::invalid_argument("unsupported controller configuration field");
        }
        if (!updates.emplace(key, value).second) {
          throw std::invalid_argument("duplicate controller configuration field");
        }
      }

      if (!config::update_config(updates)) {
        outputTree.put("status", "false");
        outputTree.put("error", "failed to persist controller configuration");
        return;
      }
      outputTree.put("status", "true");
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "SaveGamepadConfig: "sv << e.what();
      outputTree.put("status", "false");
      outputTree.put("error", "invalid controller configuration patch");
    }
  }

  void
  getHdrEnhancedConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request) || !require_localhost(response, request, "HDR configuration")) return;
    hdr_enhanced::api::get_config(response);
  }

  void
  saveHdrEnhancedConfig(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request) || !require_localhost(response, request, "HDR configuration")) return;
    hdr_enhanced::api::save_config(response, request);
  }

  void
  getHdrEnhancedStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request) || !require_localhost(response, request, "HDR status")) return;
    hdr_enhanced::api::get_status(response);
  }

  void
  maintainHdrEnhancedComponent(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request) || !require_localhost(response, request, "HDR maintenance")) return;
    hdr_enhanced::api::maintenance(response, request);
  }

  void
  saveClients(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;

    print_req(request);

    nlohmann::json output {
      {"status", false},
    };

    try {
      std::stringstream body;
      body << request->content.rdbuf();
      const auto input = nlohmann::json::parse(body.str());
      if (!input.is_object()) {
        throw std::invalid_argument("clients must be a serialized JSON array");
      }
      const auto clients_it = input.find("clients");
      if (clients_it == input.end() || !clients_it->is_string()) {
        throw std::invalid_argument("clients must be a serialized JSON array");
      }

      const auto serialized_clients = clients_it->get<std::string>();
      constexpr std::size_t max_clients_config_size { 1024 * 1024 };
      if (serialized_clients.size() > max_clients_config_size) {
        throw std::invalid_argument("clients configuration is too large");
      }

      const auto clients = nlohmann::json::parse(serialized_clients);
      if (!clients.is_array()) {
        throw std::invalid_argument("clients must contain a JSON array");
      }
      if (clients.size() > 1024) {
        throw std::invalid_argument("too many client settings");
      }

      std::set<std::string> uuids;
      for (const auto &client : clients) {
        if (!client.is_object()) {
          throw std::invalid_argument("each client setting must be an object");
        }

        if (const auto uuid_it = client.find("uuid"); uuid_it != client.end()) {
          if (!uuid_it->is_string() || uuid_it->get_ref<const std::string &>().size() > 128) {
            throw std::invalid_argument("client uuid must be a string of at most 128 bytes");
          }
          const auto &uuid = uuid_it->get_ref<const std::string &>();
          if (!uuid.empty() && !uuids.insert(uuid).second) {
            throw std::invalid_argument("client uuid must be unique");
          }
        }

        if (const auto name_it = client.find("name"); name_it != client.end() &&
            (!name_it->is_string() || name_it->get_ref<const std::string &>().size() > 256)) {
          throw std::invalid_argument("client name must be a string of at most 256 bytes");
        }

        if (const auto profile_it = client.find("hdrProfile"); profile_it != client.end()) {
          if (!profile_it->is_string()) {
            throw std::invalid_argument("HDR profile must be a string");
          }
          const auto &profile = profile_it->get_ref<const std::string &>();
          if (!profile.empty() && !display_device::color_profile::is_valid_profile_basename(profile)) {
            throw std::invalid_argument("HDR profile must be an installed .icc or .icm filename");
          }
        }

        const auto brightness_mode = client.value("hdrBrightnessMode", "auto");
        if (brightness_mode != "auto" && brightness_mode != "manual") {
          throw std::invalid_argument("HDR brightness mode must be auto or manual");
        }
        if (brightness_mode == "manual") {
          const auto read_brightness = [&](const char *field) -> float {
            const auto value = client.find(field);
            if (value == client.end() || !value->is_number()) {
              throw std::invalid_argument(std::string { "manual HDR brightness requires numeric " } + field);
            }
            const float number = value->get<float>();
            if (!std::isfinite(number)) {
              throw std::invalid_argument(std::string { field } + " must be finite");
            }
            return number;
          };

          const auto validated = hdr::validate_client_display_capabilities(
            read_brightness("hdrBrightnessMaxNits"),
            read_brightness("hdrBrightnessMinNits"),
            read_brightness("hdrBrightnessMaxFullFrameNits"));
          if (!validated.capabilities.reported) {
            throw std::invalid_argument(validated.fallback_reason);
          }
        }
      }

      if (!config::save_clients_config(serialized_clients)) {
        output["error"] = "failed to persist client settings";
      }
      else {
        output["status"] = true;
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "SaveClients: "sv << e.what();
      output["error"] = e.what();
    }

    send_response(response, output);
  }

  void
  getWebhookConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    webhook::api::get_config(std::move(response), config::sunshine.config_file);
  }

  void
  saveWebhookConfig(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;

    webhook::api::save_config(
      std::move(response),
      std::move(request),
      config::sunshine.config_file
    );
  }

  void
  testWebhook(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;

    webhook::api::test_delivery(std::move(response), std::move(request));
  }

#ifdef _WIN32
  void
  getDualSenseConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    ds5_config::api::get_config(std::move(response), config::sunshine.config_file);
  }

  void
  saveDualSenseConfig(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;
    ds5_config::api::save_config(
      std::move(response),
      std::move(request),
      config::sunshine.config_file
    );
  }
#endif

  void
  restart(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    // We may not return from this call
    platf::restart();
  }

  void
  boom(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);
    if (GetConsoleWindow() == NULL) {
      lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
      return;
    }
    lifetime::exit_sunshine(0, true);
  }

  void
  resetDisplayDevicePersistence(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    display_device::session_t::get().reset_persistence();
    outputTree.put("status", true);
  }

  void
  testMicrophone(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

#ifdef _WIN32
    const auto result = platf::audio::test_mic_redirect();
    send_response(response, nlohmann::json {
      {"success", result.success},
      {"error_code", result.error_code},
      {"backend", result.backend},
    });
#else
    send_response(response, nlohmann::json {
      {"success", false},
      {"error_code", "MIC_TEST_UNSUPPORTED"},
    });
#endif
  }

  void
  getMicrophoneStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

#ifdef _WIN32
    const auto status = platf::audio::mic_redirect_status();
    send_response(response, nlohmann::json {
      {"success", true},
      {"configured_backend", status.configured_backend},
      {"active_backend", status.active_backend},
      {"fallback_reason", status.fallback_reason},
      {"component_available", status.component_available},
      {"online", status.online},
      {"device_created", status.device_created},
      {"host_streaming", status.host_streaming},
      {"generation", status.generation},
      {"state", status.state},
      {"buffered_bytes", status.buffered_bytes},
      {"underruns", status.underruns},
      {"dropped_frames", status.dropped_frames},
      {"submit_errors", status.submit_errors},
      {"last_error", status.last_error},
      {"error_code", status.error_code},
    });
#else
    send_response(response, nlohmann::json {
      {"success", true},
      {"configured_backend", "disabled"},
      {"active_backend", ""},
      {"state", "unsupported"},
      {"component_available", false},
    });
#endif
  }

#ifdef _WIN32
  void
  getVddStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    const auto status = display_device::vdd_utils::get_vdd_status();
    send_response(response, nlohmann::json {
      {"status", true},
      {"state", status.state},
      {"installed", status.installed},
      {"running", status.running},
      {"control_available", status.control_available},
      {"monitor_active", status.monitor_active},
      {"problem_code_valid", status.problem_code_valid},
      {"problem_code", status.problem_code},
      {"version_supported", false},
    });
  }

  void
  writeVulkanHdrBridgeStatus(resp_https_t response, bool operation_status) {
    const auto bridge_status = platf::vulkan_hdr_bridge::status();
    nlohmann::json output {
      {"status", operation_status},
      {"state", bridge_status.state},
      {"message", bridge_status.message},
      {"registered", bridge_status.registered},
      {"artifacts_installed", bridge_status.artifacts_installed},
      {"display_available", bridge_status.display_available},
    };
    send_response(std::move(response), output);
  }

  void
  getVulkanHdrBridgeStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    writeVulkanHdrBridgeStatus(response, true);
  }

  void
  validateVulkanHdrBridge(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    writeVulkanHdrBridgeStatus(response, platf::vulkan_hdr_bridge::validate_now());
  }
#endif

  void
  savePassword(resp_https_t response, req_https_t request) {
    if (!authorize_browser_request(response, request)) return;
    if (!check_content_type(response, request, "application/json")) return;
    if (!config::sunshine.username.empty() && !authenticate(response, request)) return;

    print_req(request);

    std::stringstream ss;
    std::stringstream configStream;
    ss << request->content.rdbuf();

    pt::ptree inputTree, outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    try {
      pt::read_json(ss, inputTree);
      auto username = inputTree.count("currentUsername") > 0 ? inputTree.get<std::string>("currentUsername") : "";
      auto newUsername = inputTree.get<std::string>("newUsername");
      auto password = inputTree.count("currentPassword") > 0 ? inputTree.get<std::string>("currentPassword") : "";
      auto newPassword = inputTree.count("newPassword") > 0 ? inputTree.get<std::string>("newPassword") : "";
      auto confirmPassword = inputTree.count("confirmNewPassword") > 0 ? inputTree.get<std::string>("confirmNewPassword") : "";

      // Validate credential lengths
      if (newUsername.size() > 128) {
        outputTree.put("status", false);
        outputTree.put("error", "Username too long (max 128)");
        return;
      }
      if (newPassword.size() > 256) {
        outputTree.put("status", false);
        outputTree.put("error", "Password too long (max 256)");
        return;
      }

      if (newUsername.length() == 0) newUsername = username;
      if (newUsername.length() == 0) {
        outputTree.put("status", false);
        outputTree.put("error", "Invalid Username");
      }
      else {
        auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
        if (config::sunshine.username.empty() || (boost::iequals(username, config::sunshine.username) && hash == config::sunshine.password)) {
          if (newPassword.empty() || newPassword != confirmPassword) {
            outputTree.put("status", false);
            outputTree.put("error", "Password Mismatch");
          }
          else {
            http::save_user_creds(config::sunshine.credentials_file, newUsername, newPassword);
            http::reload_user_creds(config::sunshine.credentials_file);
            outputTree.put("status", true);
          }
        }
        else {
          outputTree.put("status", false);
          outputTree.put("error", "Invalid Current Credentials");
        }
      }
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePassword: "sv << e.what();
      outputTree.put("status", false);
      outputTree.put("error", e.what());
      return;
    }
  }

  void
  savePin(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    pt::ptree inputTree, outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    try {
      pt::read_json(ss, inputTree);
      std::string pin = inputTree.get<std::string>("pin");
      std::string name = inputTree.get<std::string>("name");

      // Validate PIN: must be numeric digits only, 4-8 characters
      if (pin.size() < 4 || pin.size() > 8 || !std::all_of(pin.begin(), pin.end(), ::isdigit)) {
        outputTree.put("status", false);
        outputTree.put("error", "PIN must be 4-8 digits");
        return;
      }
      // Validate client name
      if (name.empty() || name.size() > 256) {
        outputTree.put("status", false);
        outputTree.put("error", "Client name must be 1-256 characters");
        return;
      }

      bool pin_result = nvhttp::pin(pin, name);
      outputTree.put("status", pin_result);

      try {
        webhook::send_event_async(webhook::event_t{
          .type = pin_result ? webhook::event_type_t::CONFIG_PIN_SUCCESS : webhook::event_type_t::CONFIG_PIN_FAILED,
          .timestamp = webhook::get_current_timestamp(),
          .client_name = name,
          .client_ip = net::addr_to_normalized_string(request->remote_endpoint().address()),
          .server_ip = net::addr_to_normalized_string(request->local_endpoint().address())
        });
      }
      catch (...) {
        BOOST_LOG(error) << "Webhook pairing event construction failed"sv;
      }
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePin: "sv << e.what();
      outputTree.put("status", false);
      outputTree.put("error", e.what());

      try {
        webhook::send_event_async(webhook::event_t{
          .type = webhook::event_type_t::CONFIG_PIN_FAILED,
          .timestamp = webhook::get_current_timestamp(),
          .client_ip = net::addr_to_normalized_string(request->remote_endpoint().address()),
          .server_ip = net::addr_to_normalized_string(request->local_endpoint().address()),
          .extra_data = {{"error", "invalid_request"}}
        });
      }
      catch (...) {
        BOOST_LOG(error) << "Webhook pairing failure event construction failed"sv;
      }
      return;
    }
  }

  void
  getQrPairStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    nlohmann::json j;
    j["status"] = nvhttp::get_qr_pair_status();

    std::string content = j.dump();
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(SimpleWeb::StatusCode::success_ok, content, headers);
  }

  void
  generateQrPairInfo(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    pt::ptree outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    // Generate a random 4-digit PIN using OpenSSL CSPRNG
    uint16_t random_val;
    RAND_bytes(reinterpret_cast<unsigned char *>(&random_val), sizeof(random_val));
    int pin_num = random_val % 10000;
    char pin_buf[5];
    std::snprintf(pin_buf, sizeof(pin_buf), "%04d", pin_num);
    std::string pin(pin_buf);

    // Set the preset PIN in nvhttp (valid for 120 seconds)
    std::string server_name = config::nvhttp.sunshine_name;
    if (!nvhttp::set_preset_pin(pin, server_name, 120)) {
      outputTree.put("status", false);
      outputTree.put("error", "Failed to set preset PIN");
      return;
    }

    // Get server address info
    auto local_addr = net::addr_to_normalized_string(request->local_endpoint().address());
    auto port = net::map_port(nvhttp::PORT_HTTP);

    // Determine the host address for the QR code URL
    std::string host;
    if (!config::nvhttp.external_ip.empty()) {
      // User explicitly configured an external IP (could be WAN for port-forwarded setups)
      host = config::nvhttp.external_ip;
    }
    else {
      // Detect a usable LAN IP (local_endpoint may be loopback or VPN)
      host = local_addr;
      auto host_net_type = net::from_address(host);
      if (host_net_type != net::LAN) {
        std::string resolved_host;

        // Method 1: UDP connect trick to find default outgoing interface
        try {
          boost::asio::io_context io_ctx;
          boost::asio::ip::udp::socket socket(io_ctx);
          socket.connect(boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("8.8.8.8"), 53));
          auto lan_addr = socket.local_endpoint().address();
          socket.close();
          auto candidate = net::addr_to_normalized_string(lan_addr);
          if (net::from_address(candidate) == net::LAN) {
            resolved_host = candidate;
          }
        }
        catch (...) {}

#ifdef _WIN32
        // Method 2 (Windows): enumerate adapters via GetAdaptersAddresses
        if (resolved_host.empty()) {
          ULONG bufLen = 15000;
          std::vector<uint8_t> buf(bufLen);
          auto pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
          if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, pAddresses, &bufLen) == NO_ERROR) {
            for (auto adapter = pAddresses; adapter; adapter = adapter->Next) {
              if (adapter->OperStatus != IfOperStatusUp) continue;
              if (adapter->IfType == IF_TYPE_TUNNEL || adapter->IfType == IF_TYPE_PPP) continue;
              for (auto unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
                auto sin = reinterpret_cast<sockaddr_in *>(unicast->Address.lpSockaddr);
                boost::asio::ip::address_v4 addr(ntohl(sin->sin_addr.s_addr));
                auto candidate = addr.to_string();
                if (net::from_address(candidate) == net::LAN) {
                  resolved_host = candidate;
                  break;
                }
              }
              if (!resolved_host.empty()) break;
            }
          }
        }
#endif

        if (!resolved_host.empty()) {
          host = resolved_host;
          BOOST_LOG(info) << "QR pair: resolved to LAN IP " << host;
        }
        else {
          BOOST_LOG(warning) << "QR pair: could not find a LAN IP, using " << host;
        }
      }
    }

    // Build the moonlight:// URL
    std::string url = "moonlight://pair?host=" + host +
                      "&port=" + std::to_string(port) +
                      "&pin=" + pin +
                      "&name=" + server_name;

    outputTree.put("status", true);
    outputTree.put("pin", pin);
    outputTree.put("host", host);
    outputTree.put("port", port);
    outputTree.put("name", server_name);
    outputTree.put("url", url);
    outputTree.put("expires_in", 120);
  }

  void
  cancelQrPair(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    nvhttp::clear_preset_pin();

    pt::ptree outputTree;
    outputTree.put("status", true);

    std::ostringstream data;
    pt::write_json(data, outputTree);
    response->write(data.str());
  }

  void
  unpairAll(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    pt::ptree outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });
    nvhttp::erase_all_clients();
    proc::proc.terminate();
    outputTree.put("status", true);
  }

  void
  unpair(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    pt::ptree inputTree, outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    try {
      pt::read_json(ss, inputTree);
      std::string uuid = inputTree.get<std::string>("uuid");

      // Validate UUID format (hex + hyphens, reasonable length)
      if (uuid.empty() || uuid.size() > 64) {
        outputTree.put("status", false);
        outputTree.put("error", "Invalid client UUID");
        return;
      }

      const bool removed = nvhttp::unpair_client(uuid);
      outputTree.put("status", removed);

      if (removed && nvhttp::get_all_clients().empty()) {
        proc::proc.terminate();
      }
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "Unpair: "sv << e.what();
      outputTree.put("status", false);
      outputTree.put("error", e.what());
      return;
    }
  }

  void
  renameClient(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;

    print_req(request);

    pt::ptree inputTree, outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      pt::read_json(ss, inputTree);

      std::string uuid = inputTree.get<std::string>("uuid");
      std::string new_name = inputTree.get<std::string>("name");

      if (new_name.empty()) {
        outputTree.put("status", false);
        outputTree.put("error", "Name cannot be empty");
        return;
      }

      bool result = nvhttp::rename_client(uuid, new_name);
      outputTree.put("status", result);
      if (!result) {
        outputTree.put("error", "Client not found");
      }
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "Rename client: "sv << e.what();
      outputTree.put("status", false);
      outputTree.put("error", e.what());
    }
  }

  void
  listClients(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);
    nlohmann::json named_certs = nvhttp::get_all_clients();
    const auto sessions = stream::session::get_all_sessions_info();
    for (auto &client : named_certs) {
      if (!client.is_object()) continue;
      const auto uuid = client.value("uuid", std::string {});
      const auto name = client.value("name", std::string {});
      const auto session = std::find_if(sessions.begin(), sessions.end(), [&](const auto &candidate) {
        return (!uuid.empty() && candidate.client_uuid == uuid) ||
               (uuid.empty() && !name.empty() && candidate.client_name == name);
      });
      if (session == sessions.end()) continue;

      client["hdrBrightnessRuntime"] = {
        { "active", true },
        { "reported", session->hdr_brightness_reported },
        { "source", session->hdr_brightness_source },
        { "maxNits", session->hdr_max_nits },
        { "minNits", session->hdr_min_nits },
        { "maxFullFrameNits", session->hdr_max_full_frame_nits },
      };
    }
    nlohmann::json output_tree;
    output_tree["named_certs"] = named_certs;
    output_tree["status"] = "true";
    send_response(response, output_tree);
  }

  void
  listColorProfiles(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

#ifdef _WIN32
    const auto windows_directory = []() -> std::filesystem::path {
      if (const char *windir = std::getenv("WINDIR"); windir && *windir) {
        return windir;
      }
      return R"(C:\Windows)";
    }();
    const auto color_directory = windows_directory / "System32" / "spool" / "drivers" / "color";

    std::error_code error;
    std::vector<std::string> profiles;
    std::filesystem::directory_iterator entry { color_directory, std::filesystem::directory_options::skip_permission_denied, error };
    const std::filesystem::directory_iterator end;
    while (!error && entry != end) {
      std::error_code entry_error;
      if (entry->is_regular_file(entry_error) && !entry_error) {
        const auto utf8_name = entry->path().filename().u8string();
        const std::string profile_name {
          reinterpret_cast<const char *>(utf8_name.data()),
          utf8_name.size()
        };
        if (display_device::color_profile::is_valid_profile_basename(profile_name)) {
          profiles.push_back(profile_name);
        }
      }
      entry.increment(error);
    }

    if (error) {
      send_response(response, nlohmann::json {
        {"status", false},
        {"supported", true},
        {"profiles", nlohmann::json::array()},
        {"error", "failed to enumerate installed color profiles"},
      });
      return;
    }

    std::sort(profiles.begin(), profiles.end());
    send_response(response, nlohmann::json {
      {"status", true},
      {"supported", true},
      {"profiles", profiles},
    });
#else
    send_response(response, nlohmann::json {
      {"status", true},
      {"supported", false},
      {"profiles", nlohmann::json::array()},
    });
#endif
  }

  void
  closeApp(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    pt::ptree outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    proc::proc.terminate();
    outputTree.put("status", true);
  }

  void
  write_runtime_error(resp_https_t response, SimpleWeb::StatusCode http_status, int status_code, const std::string &status_message) {
    json error_json;
    error_json["success"] = false;
    error_json["status_code"] = status_code;
    error_json["status_message"] = status_message;

    response->write(http_status, error_json.dump());
    response->close_connection_after_response = true;
  }

  bool
  require_localhost(resp_https_t response, req_https_t request, const std::string &action) {
    auto client_address = request->remote_endpoint().address();
    auto address = net::addr_to_normalized_string(client_address);
    auto ip_type = net::from_address(address);

    if (ip_type == net::PC) {
      return true;
    }

    std::ostringstream msg_stream;
    msg_stream << "Access denied when " << action << ". Only localhost requests are allowed. Client IP: " << client_address.to_string();
    BOOST_LOG(warning) << msg_stream.str();
    write_runtime_error(response, SimpleWeb::StatusCode::client_error_forbidden, 403, msg_stream.str());
    return false;
  }

  void
  getRuntimeSessions(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    if (!require_localhost(response, request, "getting runtime sessions")) {
      return;
    }

    try {
      // 获取所有活动会话信息
      auto sessions_info = stream::session::get_all_sessions_info();
      
      json response_json;
      response_json["success"] = true;
      response_json["status_code"] = 200;
      response_json["status_message"] = "Success";
      response_json["total_sessions"] = sessions_info.size();
      
      json sessions_array = json::array();

      for (const auto &session_info : sessions_info) {
        json session_obj;
        session_obj["client_name"] = session_info.client_name;
        session_obj["client_uuid"] = session_info.client_uuid;
        session_obj["client_address"] = session_info.client_address;
        session_obj["state"] = session_info.state;
        session_obj["session_id"] = session_info.session_id;
        session_obj["width"] = session_info.width;
        session_obj["height"] = session_info.height;
        session_obj["fps"] = session_info.fps;
        session_obj["bitrate"] = session_info.bitrate;
        session_obj["host_audio"] = session_info.host_audio;
        session_obj["enable_hdr"] = session_info.enable_hdr;
        session_obj["enable_mic"] = session_info.enable_mic;
        session_obj["use_vdd"] = session_info.use_vdd;
        session_obj["hdr_brightness_reported"] = session_info.hdr_brightness_reported;
        session_obj["hdr_brightness_source"] = session_info.hdr_brightness_source;
        session_obj["hdr_max_nits"] = session_info.hdr_max_nits;
        session_obj["hdr_min_nits"] = session_info.hdr_min_nits;
        session_obj["hdr_max_full_frame_nits"] = session_info.hdr_max_full_frame_nits;
        session_obj["app_name"] = session_info.app_name;
        session_obj["app_id"] = session_info.app_id;
        
        sessions_array.push_back(session_obj);
      }
      
      response_json["sessions"] = sessions_array;
      
      BOOST_LOG(debug) << "Config API: Runtime sessions info requested, returned " << sessions_info.size() << " sessions";
      
      response->write(response_json.dump());
      response->close_connection_after_response = true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "getRuntimeSessions: " << e.what();
      write_runtime_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, 500, std::string(e.what()));
    }
    catch (...) {
      BOOST_LOG(error) << "getRuntimeSessions: Unknown exception";
      write_runtime_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, 500, "Unknown error");
    }
  }

  void
  getRuntimeHdrStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    if (!require_localhost(response, request, "getting runtime HDR status")) {
      return;
    }

    try {
      const auto statuses = video::get_hdr_pipeline_statuses();
      const auto enhancement_status = hdr_enhanced::manager().status();
      json response_json {
        { "success", true },
        { "status_code", 200 },
        { "status_message", "Success" },
#ifdef _WIN32
        { "available", true },
#else
        { "available", false },
#endif
        { "configured_analysis_mode", config::video.hdr_luminance_analysis },
        { "configured_conversion_mode", config::video.capture_compute_shader },
        { "configured_hdr_backend", enhancement_status.value("selected_backend", std::string {}) },
        { "pipelines", json::array() },
      };

      for (const auto &status : statuses) {
        response_json["pipelines"].push_back({
          { "id", status.id },
          { "hdr_mode", status.hdr_mode },
          { "analysis_mode", status.analysis_mode },
          { "analysis_active", status.analysis_active },
          { "scene_metadata_active", status.scene_metadata_active },
          { "metadata_formats", status.metadata_formats },
          { "conversion_path", status.conversion_path },
          { "conversion_fallback_reason", status.conversion_fallback_reason },
          { "analysis_failure_reason", status.analysis_failure_reason },
          { "synthetic_hdr_backend", status.synthetic_hdr_backend },
          { "synthetic_hdr_state", status.synthetic_hdr_state },
          { "synthetic_hdr_failure_reason", status.synthetic_hdr_failure_reason },
        });
      }

      response->write(response_json.dump());
      response->close_connection_after_response = true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "getRuntimeHdrStatus: " << e.what();
      write_runtime_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, 500, std::string(e.what()));
    }
    catch (...) {
      BOOST_LOG(error) << "getRuntimeHdrStatus: Unknown exception";
      write_runtime_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, 500, "Unknown error");
    }
  }

  void
  getRuntimeHdrCalibration(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);
    if (!require_localhost(response, request, "getting HDR calibration status")) return;

    try {
      json output {
        { "success", true },
        { "supported", false },
        { "vddActive", false },
        { "hdrEnabled", false },
        { "calibrated", false },
        { "profileName", nullptr },
        { "maxNits", nullptr },
        { "minNits", nullptr },
        { "maxFullFrameNits", nullptr },
        { "state", "unsupported" },
        { "sharedVdd", config::video.vdd_reuse },
        { "activeClients", json::array() },
      };

      for (const auto &session : stream::session::get_all_sessions_info()) {
        if (session.state == "RUNNING" && session.enable_hdr && session.use_vdd) {
          output["activeClients"].push_back({
            { "uuid", session.client_uuid },
            { "name", session.client_name },
          });
        }
      }

#ifdef _WIN32
      output["supported"] = true;
      const auto vdd_id = display_device::find_device_by_friendlyname(ZAKO_NAME);
      if (!vdd_id.empty()) {
        output["vddActive"] = true;
        const auto hdr_states = display_device::get_current_hdr_states({ vdd_id });
        const auto hdr_state = hdr_states.find(vdd_id);
        const bool hdr_enabled = hdr_state != hdr_states.end() &&
                                 hdr_state->second == display_device::hdr_state_e::enabled;
        output["hdrEnabled"] = hdr_enabled;
        output["state"] = hdr_enabled ? "ready" : "waiting_hdr";

        if (const auto calibration = display_device::win_color_profile::current_hdr_calibration(vdd_id)) {
          output["calibrated"] = true;
          output["profileName"] = calibration->profile_name;
          output["maxNits"] = calibration->max_nits;
          output["minNits"] = calibration->min_nits;
          output["maxFullFrameNits"] = calibration->max_full_frame_nits;
          output["state"] = "calibrated";
        }
      }
      else {
        output["state"] = "waiting_vdd";
      }
#endif

      send_response(response, output);
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "getRuntimeHdrCalibration: " << e.what();
      write_runtime_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, 500, e.what());
    }
    catch (...) {
      BOOST_LOG(error) << "getRuntimeHdrCalibration: Unknown exception";
      write_runtime_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, 500, "Unknown error");
    }
  }

  void
  getPerfCurrent(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    if (!require_localhost(response, request, "getting performance snapshot")) {
      return;
    }

    try {
      response->write(perf::current_snapshot_json().dump());
      response->close_connection_after_response = true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "getPerfCurrent: " << e.what();

      write_runtime_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, 500, std::string(e.what()));
    }
    catch (...) {
      BOOST_LOG(error) << "getPerfCurrent: Unknown exception";
      write_runtime_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, 500, "Unknown error");
    }
  }

  void
  changeRuntimeBitrate(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    if (!require_localhost(response, request, "changing runtime bitrate")) {
      return;
    }

    try {
      auto args = request->parse_query_string();
      auto bitrate_param = args.find("bitrate");
      auto clientname_param = args.find("clientname");

      // 验证参数
      if (bitrate_param == args.end()) {
        std::ostringstream msg_stream;
        msg_stream << "Missing bitrate parameter when changing bitrate";
        BOOST_LOG(warning) << msg_stream.str();
        write_runtime_error(response, SimpleWeb::StatusCode::client_error_bad_request, 400, msg_stream.str());
        return;
      }

      if (clientname_param == args.end()) {
        std::ostringstream msg_stream;
        msg_stream << "Missing clientname parameter when changing bitrate";
        BOOST_LOG(warning) << msg_stream.str();
        write_runtime_error(response, SimpleWeb::StatusCode::client_error_bad_request, 400, msg_stream.str());
        return;
      }

      // 安全地解析码率参数
      int bitrate = 0;
      try {
        bitrate = std::stoi(bitrate_param->second);
      }
      catch (...) {
        write_runtime_error(response, SimpleWeb::StatusCode::client_error_bad_request, 400, "Invalid bitrate parameter format");
        return;
      }

      std::string client_name = clientname_param->second;

      // 验证码率范围
      if (bitrate <= 0 || bitrate > 800000) {
        std::ostringstream msg_stream;
        msg_stream << "Invalid bitrate value when changing bitrate. Must be between 1 and 800000 Kbps";
        BOOST_LOG(warning) << msg_stream.str();
        write_runtime_error(response, SimpleWeb::StatusCode::client_error_bad_request, 400, msg_stream.str());
        return;
      }

      // 获取所有活动会话以便调试
      std::vector<std::string> available_clients;
      try {
        auto sessions_info = stream::session::get_all_sessions_info();
        for (const auto &session_info : sessions_info) {
          if (session_info.state == "RUNNING") {
            available_clients.push_back(session_info.client_name);
          }
        }
      }
      catch (...) {
        // 继续执行，即使获取会话信息失败，仍然尝试修改码率
      }
      
      BOOST_LOG(info) << "Config API: Attempting to change bitrate for client '" << client_name 
                      << "' to " << bitrate << " Kbps";
      if (!available_clients.empty()) {
        BOOST_LOG(info) << "Available RUNNING clients: " << boost::algorithm::join(available_clients, ", ");
      }
      
      // 调用底层 API 修改码率
      video::dynamic_param_t param;
      param.type = video::dynamic_param_type_e::BITRATE;
      param.value.int_value = bitrate;
      param.valid = true;
      
      bool success = stream::session::change_dynamic_param_for_client(client_name, param);

      json response_json;
      if (success) {
        response_json["success"] = true;
        response_json["status_code"] = 200;
        response_json["status_message"] = "Bitrate change request sent to client session";
        response_json["bitrate"] = bitrate;
        response_json["client_name"] = client_name;
        
        BOOST_LOG(info) << "Config API: Dynamic bitrate change requested for client '" 
                       << client_name << "': " << bitrate << " Kbps";
      } else {
        std::string error_msg = "No active streaming session found for client: " + client_name;
        if (!available_clients.empty()) {
          error_msg += ". Available clients: " + boost::algorithm::join(available_clients, ", ");
        } else {
          error_msg += ". No RUNNING sessions available.";
        }
        
        response_json["success"] = false;
        response_json["status_code"] = 404;
        response_json["status_message"] = error_msg;
        
        BOOST_LOG(warning) << "Config API: Failed to change bitrate - " << error_msg;
      }
      
      response->write(
        success ? SimpleWeb::StatusCode::success_ok : SimpleWeb::StatusCode::client_error_not_found,
        response_json.dump()
      );
      response->close_connection_after_response = true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "changeRuntimeBitrate: " << e.what();
      write_runtime_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, 500, std::string(e.what()));
    }
    catch (...) {
      BOOST_LOG(error) << "changeRuntimeBitrate: Unknown exception";
      write_runtime_error(response, SimpleWeb::StatusCode::server_error_internal_server_error, 500, "Unknown error");
    }
  }

  void
  proxySteamApi(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    // 提取请求路径，移除/steam-api前缀
    std::string path = request->path;
    if (path.find("/steam-api") == 0) {
      path = path.substr(10); // 移除"/steam-api"前缀
    }

    // 构建目标URL
    std::string targetUrl = "https://api.steampowered.com" + path;
    
    // 添加查询参数
    if (!request->query_string.empty()) {
      targetUrl += "?" + request->query_string;
    }

    BOOST_LOG(info) << "Steam API proxy request path: " << http_util::sanitize_request_log_value(path);

    // 安全检查：防止SSRF，确保目标主机确实是api.steampowered.com
    if (http::url_get_host(targetUrl) != "api.steampowered.com") {
      BOOST_LOG(warning) << "Blocked Steam API proxy request to unauthorized host";
      response->write(SimpleWeb::StatusCode::client_error_bad_request, "Invalid Host");
      return;
    }

    // 使用http模块下载数据
    std::string content;
    
    try {
      if (http::fetch_url(targetUrl, content)) {
        // 设置响应头
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        
        response->write(SimpleWeb::StatusCode::success_ok, content, headers);
      } else {
        BOOST_LOG(error) << "Steam API request failed for path: " << http_util::sanitize_request_log_value(path);
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Steam API request failed");
      }
    } catch (const std::exception& e) {
      BOOST_LOG(error) << "Steam API proxy exception: " << e.what();
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Steam API proxy exception");
    }
  }

  void
  proxySteamStore(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    // 提取请求路径，移除/steam-store前缀
    std::string path = request->path;
    if (path.find("/steam-store") == 0) {
      path = path.substr(12); // 移除"/steam-store"前缀
    }

    // 构建目标URL
    std::string targetUrl = "https://store.steampowered.com" + path;
    
    // 添加查询参数
    if (!request->query_string.empty()) {
      targetUrl += "?" + request->query_string;
    }

    BOOST_LOG(info) << "Steam Store proxy request path: " << http_util::sanitize_request_log_value(path);

    // 安全检查：防止SSRF，确保目标主机确实是store.steampowered.com
    if (http::url_get_host(targetUrl) != "store.steampowered.com") {
      BOOST_LOG(warning) << "Blocked Steam Store proxy request to unauthorized host";
      response->write(SimpleWeb::StatusCode::client_error_bad_request, "Invalid Host");
      return;
    }

    // 使用http模块下载数据
    std::string content;
    
    try {
      if (http::fetch_url(targetUrl, content)) {
        // 设置响应头
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        
        response->write(SimpleWeb::StatusCode::success_ok, content, headers);
      } else {
        BOOST_LOG(error) << "Steam Store request failed for path: " << http_util::sanitize_request_log_value(path);
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Steam Store request failed");
      }
    } catch (const std::exception& e) {
      BOOST_LOG(error) << "Steam Store proxy exception: " << e.what();
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Steam Store proxy exception");
    }
  }

  // ===== AI LLM Proxy =====

  // 内存缓存 AI 配置，避免每次请求都读文件
  static std::mutex ai_config_mutex;
  static nlohmann::json ai_config_cache;
  static bool ai_config_loaded = false;

  /**
   * @brief 获取 AI 配置文件路径（与 sunshine.conf 同目录）
   */
  static fs::path
  getAiConfigPath() {
    auto config_dir = file_handler::path_from_utf8(config::sunshine.config_file).parent_path();
    return config_dir / "ai_config.json";
  }

  static fs::path
  getAiCredentialPath() {
    auto config_dir = file_handler::path_from_utf8(config::sunshine.config_file).parent_path();
    return config_dir / "ai_llm_credential.bin";
  }

  static void
  applyStoredCredentialLocked(nlohmann::json &cfg) {
    auto credential = credential_store::read_llm_api_key(getAiCredentialPath());
    if (credential.status == credential_store::read_status_e::success) {
      cfg["apiKey"] = std::move(credential.secret);
    } else {
      cfg["apiKey"] = "";
      if (credential.status == credential_store::read_status_e::error) {
        BOOST_LOG(error) << "Could not load the LLM API key from secure storage: " << credential.error;
      }
    }
    cfg["apiKeyConfigured"] = !cfg.value("apiKey", "").empty();
  }

  static bool
  writeAiConfigFile(const nlohmann::json &cfg) {
    const fs::path path = getAiConfigPath();
    auto temp = path;
    temp += ".tmp";
    try {
      {
        std::ofstream file(temp, std::ios::trunc);
        if (!file.is_open()) return false;
        file << cfg.dump(2);
        file.flush();
        if (!file.good()) throw std::runtime_error("Failed to flush AI config");
      }
#ifdef _WIN32
      if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        return false;
      }
#else
      std::error_code ec;
      fs::rename(temp, path, ec);
      if (ec) {
        fs::remove(temp, ec);
        return false;
      }
#endif
      return true;
    } catch (const std::exception &e) {
      std::error_code ignored;
      fs::remove(temp, ignored);
      BOOST_LOG(error) << "Failed to save AI config: " << e.what();
      return false;
    }
  }

  /**
   * @brief 从文件或缓存读取 AI 配置（调用方需持有 ai_config_mutex）
   */
  static nlohmann::json
  loadAiConfigLocked() {
    if (ai_config_loaded) {
      return ai_config_cache;
    }

    auto path = getAiConfigPath();
    try {
      const auto utf8_path = file_handler::path_to_utf8(path);
      std::string content = file_handler::read_file(utf8_path.c_str());
      if (!content.empty()) {
        auto persisted = nlohmann::json::parse(content);
        ai_config_cache = persisted;

        // Migrate legacy plaintext credentials before exposing the runtime
        // configuration. Never remove the old value until both secure storage
        // and the sanitized config rewrite have succeeded.
        const std::string legacy_key = persisted.value("apiKey", "");
        if (!legacy_key.empty()) {
          auto migration = credential_store::write_llm_api_key(getAiCredentialPath(), legacy_key);
          if (migration.success) {
            auto sanitized = persisted;
            sanitized.erase("apiKey");
            if (writeAiConfigFile(sanitized)) {
              ai_config_cache = std::move(sanitized);
              BOOST_LOG(info) << "Migrated the LLM API key out of ai_config.json";
            } else {
              BOOST_LOG(error) << "The LLM API key was secured, but ai_config.json could not be sanitized; migration will retry";
            }
          } else {
            BOOST_LOG(error) << "Could not migrate the plaintext LLM API key: " << migration.error;
          }
          ai_config_cache["apiKey"] = legacy_key;
        } else {
          applyStoredCredentialLocked(ai_config_cache);
        }
        ai_config_cache["apiKeyConfigured"] = !ai_config_cache.value("apiKey", "").empty();
        ai_config_loaded = true;
        return ai_config_cache;
      }
    } catch (...) {}

    ai_config_cache = nlohmann::json{
      {"enabled", false},
      {"provider", "openai"},
      {"apiBase", "https://api.openai.com/v1"},
      {"apiKey", ""},
      {"apiKeyConfigured", false},
      {"model", "gpt-4.1-mini"},
      {"compatibility", "openai-chat"},
      {"temperature", 0.3},
      {"max_tokens", 2048}
    };
    applyStoredCredentialLocked(ai_config_cache);
    ai_config_loaded = true;
    return ai_config_cache;
  }

  /**
   * @brief 从文件或缓存读取 AI 配置（线程安全）
   */
  static nlohmann::json
  loadAiConfig() {
    std::lock_guard<std::mutex> lock(ai_config_mutex);
    return loadAiConfigLocked();
  }

  /**
   * @brief 保存 AI 配置并刷新缓存（调用方需持有 ai_config_mutex）
   */
  static bool
  saveAiConfigLocked(const nlohmann::json &cfg) {
    auto persisted = cfg;
    persisted.erase("apiKey");
    persisted.erase("apiKeyConfigured");
    persisted.erase("apiKeyHint");
    if (!writeAiConfigFile(persisted)) return false;
    ai_config_cache = cfg;
    ai_config_loaded = true;
    return true;
  }

  /**
   * @brief 检测 provider 是否为 Anthropic（需要不同的 API 格式）
   */
  static bool
  isAnthropicProvider(const nlohmann::json &cfg) {
    std::string compatibility = cfg.value("compatibility", "");
    std::string provider = cfg.value("provider", "");
    std::string apiBase = cfg.value("apiBase", "");
    return compatibility == "anthropic-messages" || provider == "anthropic" || apiBase.find("anthropic.com") != std::string::npos;
  }

  static bool
  isApiKeyRequired(const nlohmann::json &cfg) {
    std::string provider = cfg.value("provider", "");
    std::string apiBase = cfg.value("apiBase", "");
    return provider != "ollama" &&
           apiBase.find("localhost") == std::string::npos &&
           apiBase.find("127.0.0.1") == std::string::npos &&
           apiBase.find("[::1]") == std::string::npos;
  }

  static bool
  hasSuffix(const std::string &value, const std::string &suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  static std::string
  buildAiEndpoint(std::string apiBase, bool isAnthropic) {
    while (!apiBase.empty() && apiBase.back() == '/') {
      apiBase.pop_back();
    }

    if (isAnthropic) {
      if (hasSuffix(apiBase, "/v1/messages") || hasSuffix(apiBase, "/messages")) {
        return apiBase;
      }
      if (hasSuffix(apiBase, "/v1")) {
        return apiBase + "/messages";
      }
      return apiBase + "/v1/messages";
    }

    if (hasSuffix(apiBase, "/chat/completions")) {
      return apiBase;
    }
    return apiBase + "/chat/completions";
  }

  /**
   * @brief 将 OpenAI 格式的请求转换为 Anthropic 格式
   */
  static std::string
  convertToAnthropicFormat(const std::string &openaiBody, const std::string &model) {
    try {
      auto input = nlohmann::json::parse(openaiBody);
      nlohmann::json anthropic;

      anthropic["model"] = input.value("model", model);
      anthropic["max_tokens"] = input.value("max_tokens", 4096);

      // 提取 system message 和 user/assistant messages
      if (input.contains("messages")) {
        nlohmann::json messages = nlohmann::json::array();
        for (auto &msg : input["messages"]) {
          std::string role = msg.value("role", "");
          if (role == "system") {
            anthropic["system"] = msg.value("content", "");
          } else {
            messages.push_back(msg);
          }
        }
        anthropic["messages"] = messages;
      }

      // 转换 temperature, top_p 等通用参数
      if (input.contains("temperature")) anthropic["temperature"] = input["temperature"];
      if (input.contains("top_p")) anthropic["top_p"] = input["top_p"];
      if (input.contains("stream")) anthropic["stream"] = input["stream"];

      return anthropic.dump();
    } catch (...) {
      return openaiBody;  // 转换失败，原样返回
    }
  }

  /**
   * @brief 将 Anthropic 格式的响应转换回 OpenAI 格式
   */
  static std::string
  convertFromAnthropicFormat(const std::string &anthropicResponse) {
    try {
      auto resp = nlohmann::json::parse(anthropicResponse);
      nlohmann::json openai;

      openai["id"] = resp.value("id", "");
      openai["object"] = "chat.completion";
      openai["model"] = resp.value("model", "");

      // 转换 content blocks
      nlohmann::json choice;
      choice["index"] = 0;
      choice["finish_reason"] = resp.value("stop_reason", "stop");

      std::string content;
      if (resp.contains("content") && resp["content"].is_array()) {
        for (auto &block : resp["content"]) {
          if (block.value("type", "") == "text") {
            content += block.value("text", "");
          }
        }
      }
      choice["message"]["role"] = "assistant";
      choice["message"]["content"] = content;
      openai["choices"] = nlohmann::json::array({choice});

      // 转换 usage
      if (resp.contains("usage")) {
        openai["usage"]["prompt_tokens"] = resp["usage"].value("input_tokens", 0);
        openai["usage"]["completion_tokens"] = resp["usage"].value("output_tokens", 0);
        openai["usage"]["total_tokens"] =
          resp["usage"].value("input_tokens", 0) + resp["usage"].value("output_tokens", 0);
      }

      return openai.dump();
    } catch (...) {
      return anthropicResponse;
    }
  }

  SimpleWeb::CaseInsensitiveMultimap
  json_headers() {
    return {
      { "Content-Type", "application/json" },
      { "Cache-Control", "no-store" },
      { "X-Content-Type-Options", "nosniff" }
    };
  }

  void
  write_json(resp_https_t response, SimpleWeb::StatusCode status, const json &body) {
    response->write(status, body.dump(), json_headers());
  }

  void
  write_json_error(resp_https_t response, SimpleWeb::StatusCode status, std::string error) {
    json body;
    body["ok"] = false;
    body["error"] = std::move(error);
    write_json(std::move(response), status, body);
  }

  bool
  read_json_body(resp_https_t response, req_https_t request, json &body) {
    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      body = json::parse(ss.str());
      return true;
    }
    catch (const std::exception &e) {
      write_json_error(std::move(response), SimpleWeb::StatusCode::client_error_bad_request, std::string { "Invalid JSON: " } + e.what());
      return false;
    }
  }

  bool
  persistFileMappingStoreOrRestore(std::vector<file_mapping::mapping_t> previous_mappings) {
    if (file_mapping_store::persist_to_config(file_mapping_store::global())) {
      return true;
    }

    file_mapping_store::global().replace(std::move(previous_mappings));
    return false;
  }

  void
  listFileMappings(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    json body;
    body["ok"] = true;
    body["mappings"] = file_mapping_store::global().to_json();
    write_json(std::move(response), SimpleWeb::StatusCode::success_ok, body);
  }

  void
  createFileMapping(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;
    print_req(request);

    json input;
    if (!read_json_body(response, request, input)) return;
    if (!input.is_object() || !input.contains("path") || !input["path"].is_string()) {
      write_json_error(std::move(response), SimpleWeb::StatusCode::client_error_bad_request, "missing string field: path");
      return;
    }

    file_mapping_store::mutation_result_t result;
    {
      std::scoped_lock transaction_lock { file_mapping_store_transaction_mutex };
      auto previous_mappings = file_mapping_store::global().snapshot();
      result = file_mapping_store::global().add_quick_share(input["path"].get<std::string>());
      if (!result.ok) {
        write_json_error(std::move(response), SimpleWeb::StatusCode::client_error_bad_request, result.error);
        return;
      }
      if (!persistFileMappingStoreOrRestore(std::move(previous_mappings))) {
        write_json_error(std::move(response), SimpleWeb::StatusCode::server_error_internal_server_error, "failed to persist mapping configuration");
        return;
      }
    }

    json body;
    body["ok"] = true;
    body["mapping"] = file_mapping_store::mapping_to_config_json(result.mapping);
    write_json(std::move(response), SimpleWeb::StatusCode::success_ok, body);
  }

  void
  updateFileMapping(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;
    print_req(request);

    const auto id = request->path_match.size() > 1 ? request->path_match[1].str() : std::string {};
    if (!file_mapping::is_valid_mapping_id(id)) {
      write_json_error(std::move(response), SimpleWeb::StatusCode::client_error_bad_request, "invalid mapping id");
      return;
    }

    json input;
    if (!read_json_body(response, request, input)) return;
    file_mapping_store::mutation_result_t result;
    {
      std::scoped_lock transaction_lock { file_mapping_store_transaction_mutex };
      auto previous_mappings = file_mapping_store::global().snapshot();
      result = file_mapping_store::global().update(id, input);
      if (!result.ok) {
        const auto status = result.error == "mapping was not found" ? SimpleWeb::StatusCode::client_error_not_found : SimpleWeb::StatusCode::client_error_bad_request;
        write_json_error(std::move(response), status, result.error);
        return;
      }
      if (!persistFileMappingStoreOrRestore(std::move(previous_mappings))) {
        write_json_error(std::move(response), SimpleWeb::StatusCode::server_error_internal_server_error, "failed to persist mapping configuration");
        return;
      }
    }

    json body;
    body["ok"] = true;
    body["mapping"] = file_mapping_store::mapping_to_config_json(result.mapping);
    write_json(std::move(response), SimpleWeb::StatusCode::success_ok, body);
  }

  void
  deleteFileMapping(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    const auto id = request->path_match.size() > 1 ? request->path_match[1].str() : std::string {};
    if (!file_mapping::is_valid_mapping_id(id)) {
      write_json_error(std::move(response), SimpleWeb::StatusCode::client_error_bad_request, "invalid mapping id");
      return;
    }
    {
      std::scoped_lock transaction_lock { file_mapping_store_transaction_mutex };
      auto previous_mappings = file_mapping_store::global().snapshot();
      if (!file_mapping_store::global().remove(id)) {
        write_json_error(std::move(response), SimpleWeb::StatusCode::client_error_not_found, "mapping was not found");
        return;
      }
      if (!persistFileMappingStoreOrRestore(std::move(previous_mappings))) {
        write_json_error(std::move(response), SimpleWeb::StatusCode::server_error_internal_server_error, "failed to persist mapping configuration");
        return;
      }
    }

    json body;
    body["ok"] = true;
    body["id"] = id;
    write_json(std::move(response), SimpleWeb::StatusCode::success_ok, body);
  }

  /**
   * @brief GET /api/ai/config — 获取 AI 配置（不返回完整 API key）
   */
  void
  getAiConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    auto cfg = loadAiConfig();

    // Never expose the key; only report whether one is configured.
    const std::string key = cfg.value("apiKey", "");
    cfg.erase("apiKey");
    cfg["apiKeyConfigured"] = !key.empty();

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(SimpleWeb::StatusCode::success_ok, cfg.dump(), headers);
  }

  /**
   * @brief POST /api/ai/config — 保存 AI 配置
   */
  void
  saveAiConfigEndpoint(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;
    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    nlohmann::json output;
    try {
      auto input = nlohmann::json::parse(ss.str());

      // 用同一把锁包住 load-modify-save，防止并发写入丢失
      std::lock_guard<std::mutex> lock(ai_config_mutex);
      auto current = loadAiConfigLocked();
      if (input.contains("enabled")) current["enabled"] = input["enabled"].get<bool>();
      if (input.contains("provider")) current["provider"] = input["provider"].get<std::string>();
      if (input.contains("apiBase")) current["apiBase"] = input["apiBase"].get<std::string>();
      if (input.contains("model")) current["model"] = input["model"].get<std::string>();
      if (input.contains("compatibility")) current["compatibility"] = input["compatibility"].get<std::string>();
      if (input.contains("system_prompt")) current["system_prompt"] = input["system_prompt"].get<std::string>();
      if (input.contains("temperature")) current["temperature"] = input["temperature"].get<double>();
      if (input.contains("max_tokens")) current["max_tokens"] = input["max_tokens"].get<int>();
      std::string key_action = input.value("apiKeyAction", "keep");
      // Backward compatibility for older clients that only send apiKey.
      if (input.contains("apiKey") && key_action == "keep") {
        const std::string candidate = input["apiKey"].get<std::string>();
        if (!candidate.empty() && candidate.find("****") == std::string::npos) {
          key_action = "replace";
        }
      }

      if (key_action == "replace") {
        if (!input.contains("apiKey") || !input["apiKey"].is_string() || input["apiKey"].get_ref<const std::string &>().empty()) {
          throw std::invalid_argument("apiKeyAction=replace requires a non-empty apiKey");
        }
        const std::string key = input["apiKey"].get<std::string>();
        auto stored = credential_store::write_llm_api_key(getAiCredentialPath(), key);
        if (!stored.success) throw std::runtime_error("Failed to secure API key: " + stored.error);
        current["apiKey"] = key;
      } else if (key_action == "clear") {
        auto erased = credential_store::erase_llm_api_key(getAiCredentialPath());
        if (!erased.success) throw std::runtime_error("Failed to clear API key: " + erased.error);
        current["apiKey"] = "";
      } else if (key_action != "keep") {
        throw std::invalid_argument("apiKeyAction must be keep, replace, or clear");
      }
      current["apiKeyConfigured"] = !current.value("apiKey", "").empty();

      if (saveAiConfigLocked(current)) {
        output["status"] = "ok";
        output["apiKeyConfigured"] = current["apiKeyConfigured"];
      } else {
        output["status"] = "error";
        output["error"] = "Failed to write config file";
        // Credential mutations have already completed. Keep runtime behavior
        // consistent with secure storage even if the non-secret JSON write
        // failed; the caller still receives an error and can retry.
        ai_config_cache = current;
        ai_config_loaded = true;
      }
    } catch (const std::exception &e) {
      output["status"] = "error";
      output["error"] = e.what();
    }

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(SimpleWeb::StatusCode::success_ok, output.dump(), headers);
  }

  void
  proxyAiModels(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    const auto cfg = loadAiConfig();
    if (!cfg.value("enabled", false)) {
      response->write(
        SimpleWeb::StatusCode::client_error_forbidden,
        R"({"error":{"message":"AI proxy is not enabled","type":"invalid_request_error"}})",
        json_headers());
      return;
    }
    const std::string api_key = cfg.value("apiKey", "");
    std::string api_base = cfg.value("apiBase", "");
    if (api_base.empty() || (api_key.empty() && isApiKeyRequired(cfg))) {
      response->write(
        SimpleWeb::StatusCode::client_error_bad_request,
        R"({"error":{"message":"AI proxy not configured","type":"invalid_request_error"}})",
        json_headers());
      return;
    }
    if (isAnthropicProvider(cfg)) {
      response->write(SimpleWeb::StatusCode::success_ok, R"({"data":[]})", json_headers());
      return;
    }

    while (!api_base.empty() && api_base.back() == '/') api_base.pop_back();
    if (hasSuffix(api_base, "/chat/completions")) {
      api_base.resize(api_base.size() - std::string_view { "/chat/completions" }.size());
    }
    const std::string target_url = api_base + "/models";
    std::map<std::string, std::string> headers;
    if (!api_key.empty()) headers["Authorization"] = "Bearer " + api_key;

    std::string body;
    long http_code = 0;
    if (!http::get_json(target_url, headers, body, http_code)) {
      response->write(
        SimpleWeb::StatusCode::server_error_bad_gateway,
        R"({"error":{"message":"Failed to connect to upstream LLM API","type":"upstream_error"}})",
        json_headers());
      return;
    }
    const auto status = http_code >= 200 && http_code < 300
                          ? SimpleWeb::StatusCode::success_ok
                          : SimpleWeb::StatusCode::server_error_bad_gateway;
    response->write(status, body, json_headers());
  }

  /**
   * @brief POST /api/ai/chat/completions — OpenAI 兼容的 LLM 代理端点
   *
   * 支持普通请求和 SSE 流式请求。
   * 自动适配 Anthropic API 格式。
   * 客户端无需知道 API key 或实际后端，Sunshine 作为透明代理。
   */
  void
  proxyAiChat(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;
    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    std::string requestBody = ss.str();

    // 检测是否请求流式输出
    bool isStream = false;
    try {
      auto reqJson = nlohmann::json::parse(requestBody);
      isStream = reqJson.value("stream", false);
    } catch (...) {}

    if (isStream) {
      bool headerSent = false;
      auto result = processAiChatStream(requestBody, [&](const char *data, size_t len) {
        if (!headerSent) {
          *response << "HTTP/1.1 200 OK\r\n";
          *response << "Content-Type: text/event-stream\r\n";
          *response << "Cache-Control: no-cache\r\n";
          *response << "Connection: keep-alive\r\n";
          *response << "\r\n";
          response->send();
          headerSent = true;
        }
        std::string chunk(data, len);
        *response << chunk;
        response->send();
      });

      if (result.httpCode != 200 && !headerSent) {
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_bad_gateway, result.body, headers);
      }
    } else {
      auto result = processAiChat(requestBody);
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", result.contentType);

      auto statusCode = (result.httpCode == 200)
        ? SimpleWeb::StatusCode::success_ok
        : (result.httpCode == 403)
          ? SimpleWeb::StatusCode::client_error_forbidden
          : (result.httpCode == 400)
            ? SimpleWeb::StatusCode::client_error_bad_request
            : SimpleWeb::StatusCode::server_error_bad_gateway;

      response->write(statusCode, result.body, headers);
    }
  }

  /**
   * @brief OPTIONS handler for CORS preflight on AI endpoints
   */
  void
  handleAiCors(resp_https_t response, req_https_t request) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    headers.emplace("Access-Control-Allow-Headers", "Content-Type, Authorization");
    response->write(SimpleWeb::StatusCode::success_no_content, "", headers);
  }

  // ===== AI Proxy shared interface =====

  bool
  isAiEnabled() {
    auto cfg = loadAiConfig();
    return cfg.value("enabled", false) &&
           (!isApiKeyRequired(cfg) || !cfg.value("apiKey", "").empty()) &&
           !cfg.value("apiBase", "").empty();
  }

  /**
   * @brief Prepare AI proxy request: validate config, build URL, headers, convert body.
   * @return empty targetUrl on error (result is filled with error info)
   */
  static bool
  prepareAiRequest(
    const std::string &requestBody,
    std::string &targetUrl,
    std::string &processedBody,
    std::map<std::string, std::string> &proxyHeaders,
    bool &isAnthropic,
    bool &isStream,
    AiProxyResult &result) {

    auto cfg = loadAiConfig();

    if (!cfg.value("enabled", false)) {
      result = {403, R"({"error":{"message":"AI proxy is not enabled","type":"invalid_request_error"}})", "application/json"};
      return false;
    }

    std::string apiBase = cfg.value("apiBase", "");
    std::string apiKey = cfg.value("apiKey", "");
    std::string defaultModel = cfg.value("model", "");

    if (apiBase.empty() || (apiKey.empty() && isApiKeyRequired(cfg))) {
      result = {400, R"({"error":{"message":"AI proxy not configured: missing apiBase or apiKey","type":"invalid_request_error"}})", "application/json"};
      return false;
    }

    if (requestBody.empty()) {
      result = {400, R"({"error":{"message":"Empty request body","type":"invalid_request_error"}})", "application/json"};
      return false;
    }

    processedBody = requestBody;
    isStream = false;

    try {
      auto reqJson = nlohmann::json::parse(processedBody);
      isStream = reqJson.value("stream", false);
      if (!reqJson.contains("model") && !defaultModel.empty()) {
        reqJson["model"] = defaultModel;
        processedBody = reqJson.dump();
      }
    } catch (...) {}

    isAnthropic = isAnthropicProvider(cfg);

    targetUrl = buildAiEndpoint(apiBase, isAnthropic);

    if (isAnthropic) {
      // Anthropic 流式 SSE 格式与 OpenAI 不兼容，强制走非流式以保证响应格式一致
      if (isStream) {
        try {
          auto reqJson = nlohmann::json::parse(processedBody);
          reqJson["stream"] = false;
          processedBody = reqJson.dump();
        } catch (...) {}
        isStream = false;
      }
      processedBody = convertToAnthropicFormat(processedBody, defaultModel);
      if (!apiKey.empty()) {
        proxyHeaders["x-api-key"] = apiKey;
      }
      proxyHeaders["anthropic-version"] = "2023-06-01";
    } else {
      if (!apiKey.empty()) {
        proxyHeaders["Authorization"] = "Bearer " + apiKey;
      }
    }

    BOOST_LOG(info) << "AI proxy forwarding to: " << targetUrl << (isStream ? " (stream)" : "");
    return true;
  }

  AiProxyResult
  processAiChat(const std::string &requestBody) {
    std::string targetUrl, processedBody;
    std::map<std::string, std::string> proxyHeaders;
    bool isAnthropic = false, isStream = false;
    AiProxyResult result;

    if (!prepareAiRequest(requestBody, targetUrl, processedBody, proxyHeaders, isAnthropic, isStream, result)) {
      return result;
    }

    std::string responseBody;
    long httpCode = 0;

    try {
      bool ok = http::post_json(targetUrl, processedBody, proxyHeaders, responseBody, httpCode, 120);
      if (ok) {
        if (isAnthropic && httpCode >= 200 && httpCode < 300) {
          responseBody = convertFromAnthropicFormat(responseBody);
        }
        int statusCode = (httpCode >= 200 && httpCode < 300) ? 200 : 502;
        return {statusCode, responseBody, "application/json"};
      } else {
        return {502, R"({"error":{"message":"Failed to connect to upstream LLM API","type":"upstream_error"}})", "application/json"};
      }
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "AI proxy exception: " << e.what();
      nlohmann::json err;
      err["error"]["message"] = std::string("AI proxy exception: ") + e.what();
      err["error"]["type"] = "internal_error";
      return {500, err.dump(), "application/json"};
    }
  }

  /**
   * @brief curl write callback for streaming with std::function
   */
  struct StreamCallbackContext {
    std::function<void(const char *, size_t)> callback;
  };

  static size_t
  stream_func_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t realsize = size * nmemb;
    auto *ctx = static_cast<StreamCallbackContext *>(userdata);
    ctx->callback(ptr, realsize);
    return realsize;
  }

  AiProxyResult
  processAiChatStream(
    const std::string &requestBody,
    std::function<void(const char *, size_t)> chunkCallback) {

    std::string targetUrl, processedBody;
    std::map<std::string, std::string> proxyHeaders;
    bool isAnthropic = false, isStream = false;
    AiProxyResult result;

    if (!prepareAiRequest(requestBody, targetUrl, processedBody, proxyHeaders, isAnthropic, isStream, result)) {
      return result;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
      return {500, R"({"error":{"message":"Failed to initialize CURL","type":"internal_error"}})", "application/json"};
    }

    StreamCallbackContext ctx;
    ctx.callback = std::move(chunkCallback);

    struct curl_slist *header_list = nullptr;
    header_list = curl_slist_append(header_list, "Content-Type: application/json");
    for (const auto &[key, value] : proxyHeaders) {
      std::string header_line = key + ": " + value;
      header_list = curl_slist_append(header_list, header_line.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, targetUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, processedBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(processedBody.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_func_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    CURLcode curlResult = curl_easy_perform(curl);
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    if (curlResult != CURLE_OK) {
      return {502, R"({"error":{"message":"Failed to connect to upstream LLM API","type":"upstream_error"}})", "application/json"};
    }
    return {200, "", "text/event-stream"};
  }

  /**
   * @brief 计算文件的SHA256哈希值
   * @param filepath 文件路径
   * @return SHA256哈希字符串，如果失败返回空字符串
   */
  std::string
  calculate_file_hash(const std::string &filepath) {
    const auto native_path = file_handler::path_from_utf8(filepath);
    if (filepath.empty() || !fs::exists(native_path)) {
      return "";
    }

    std::ifstream file(native_path, std::ios::binary);
    if (!file.is_open()) {
      return "";
    }

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
      return "";
    }

    if (EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr) != 1) {
      EVP_MD_CTX_free(mdctx);
      return "";
    }

    char buf[1024 * 16];
    while (file.good()) {
      file.read(buf, sizeof(buf));
      if (file.gcount() > 0) {
        if (EVP_DigestUpdate(mdctx, buf, file.gcount()) != 1) {
          EVP_MD_CTX_free(mdctx);
          file.close();
          return "";
        }
      }
    }
    file.close();

    unsigned char hash[SHA256_DIGEST_LENGTH];
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(mdctx, hash, &hash_len) != 1) {
      EVP_MD_CTX_free(mdctx);
      return "";
    }
    EVP_MD_CTX_free(mdctx);

    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < hash_len; i++) {
      ss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return ss.str();
  }

  /**
   * @brief 从命令字符串中提取可执行文件路径
   * @param cmd 完整命令字符串
   * @return 可执行文件路径
   */
  std::string
  extract_executable_path(const std::string &cmd) {
    if (cmd.empty()) {
      return "";
    }

    std::string trimmed = cmd;
    // 移除前导空格
    size_t start = trimmed.find_first_not_of(" \t");
    if (start != std::string::npos) {
      trimmed = trimmed.substr(start);
    }

    // 处理引号包裹的路径
    if (!trimmed.empty() && trimmed[0] == '"') {
      size_t end = trimmed.find('"', 1);
      if (end != std::string::npos) {
        return trimmed.substr(1, end - 1);
      }
    }

    // 提取第一个空格前的部分（可执行文件路径）
    size_t space = trimmed.find(' ');
    if (space != std::string::npos) {
      return trimmed.substr(0, space);
    }

    return trimmed;
  }

  void
  testMenuCmd(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) return;
    if (!authenticate(response, request)) return;

    constexpr bool test_command_enabled = false;
    if (!test_command_enabled) {
      const nlohmann::json body {
        { "status", false },
        { "error", "Test command execution is temporarily disabled" },
        { "error_code", "TEST_COMMAND_DISABLED" },
      };
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(SimpleWeb::StatusCode::server_error_service_unavailable, body.dump(), headers);
      return;
    }

    // 安全限制：只允许局域网访问测试命令功能
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto ip_type = net::from_address(address);
    
    if (ip_type != net::PC) {
      BOOST_LOG(warning) << "TestMenuCmd: Access denied from non-local network: " << address;
      pt::ptree outputTree;
      outputTree.put("status", false);
      outputTree.put("error", "Test command feature is only available from local network");
      
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    pt::ptree inputTree, outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    try {
      pt::read_json(ss, inputTree);
      auto cmd = inputTree.get<std::string>("cmd");
      auto working_dir = inputTree.get<std::string>("working_dir", "");
      auto elevated = inputTree.get<bool>("elevated", false);

      // 安全检查：命令不能为空
      if (cmd.empty()) {
        BOOST_LOG(warning) << "TestMenuCmd: Empty command provided";
        outputTree.put("status", false);
        outputTree.put("error", "Command cannot be empty");
        return;
      }

      // 安全检查：命令长度限制（防止过长的命令）
      if (cmd.length() > 4096) {
        BOOST_LOG(warning) << "TestMenuCmd: Command too long (" << cmd.length() << " characters)";
        outputTree.put("status", false);
        outputTree.put("error", "Command exceeds maximum length");
        return;
      }

      // 提取可执行文件路径并计算SHA256哈希值
      std::string executable_path = extract_executable_path(cmd);
      std::string file_hash;
      
      if (!executable_path.empty()) {
        // 如果是相对路径，尝试解析为绝对路径
        auto exec_path = file_handler::path_from_utf8(executable_path);
        if (!exec_path.is_absolute()) {
          // 在PATH中查找或使用工作目录
          if (!working_dir.empty()) {
            exec_path = file_handler::path_from_utf8(working_dir) / exec_path;
          }
        }
        
        file_hash = calculate_file_hash(file_handler::path_to_utf8(exec_path));
        
        if (file_hash.empty() && fs::exists(exec_path)) {
          BOOST_LOG(warning) << "TestMenuCmd: Failed to calculate hash for executable: " << file_handler::path_to_utf8(exec_path);
        }
      }

      // 记录详细信息用于审计（包含文件哈希值）
      BOOST_LOG(info) << "Testing menu command from " << address << ": [" << cmd << "]";
      if (!file_hash.empty()) {
        BOOST_LOG(info) << "Executable SHA256: " << file_hash << " (" << executable_path << ")";
      }
      else if (!executable_path.empty()) {
        BOOST_LOG(warning) << "Could not verify executable: " << executable_path;
      }

      std::error_code ec;
      boost::filesystem::path work_dir;
      
      if (!working_dir.empty()) {
        const auto native_working_dir = file_handler::path_from_utf8(working_dir);
        // 验证工作目录是否存在
        if (!fs::exists(native_working_dir) || !fs::is_directory(native_working_dir)) {
          BOOST_LOG(warning) << "TestMenuCmd: Invalid working directory: " << working_dir;
          outputTree.put("status", false);
          outputTree.put("error", "Invalid working directory");
          return;
        }
#ifdef _WIN32
        work_dir = boost::filesystem::path(native_working_dir.wstring());
#else
        work_dir = boost::filesystem::path(native_working_dir.string());
#endif
      } else {
        work_dir = boost::filesystem::current_path();
      }

      // 执行命令
      auto child = platf::run_command(elevated, true, cmd, work_dir, proc::proc.get_env(), nullptr, ec, nullptr);
      
      if (ec) {
        BOOST_LOG(warning) << "Failed to run menu command [" << cmd << "]: " << ec.message();
        outputTree.put("status", false);
        outputTree.put("error", "Failed to execute command: " + ec.message());
      }
      else {
        BOOST_LOG(info) << "Successfully executed menu command [" << cmd << "]";
        child.detach();
        outputTree.put("status", true);
        outputTree.put("message", "Command executed successfully");
      }
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "TestMenuCmd error: " << e.what();
      outputTree.put("status", false);
      outputTree.put("error", e.what());
      return;
    }
  }

  void
  start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    https_server_t server { config::nvhttp.cert, config::nvhttp.pkey };
    server.default_resource["GET"] = close_connection;
    server.resource["^/$"]["GET"] = getIndexPage;
    server.resource["^/pin/?$"]["GET"] = getPinPage;
    server.resource["^/apps/?$"]["GET"] = getAppsPage;
    server.resource["^/clients/?$"]["GET"] = getClientsPage;
    server.resource["^/config/?$"]["GET"] = getConfigPage;
    server.resource["^/password/?$"]["GET"] = getPasswordPage;
    server.resource["^/welcome/?$"]["GET"] = getWelcomePage;
    server.resource["^/troubleshooting/?$"]["GET"] = getTroubleshootingPage;
    server.resource["^/api/pin$"]["POST"] = savePin;
    server.resource["^/api/qr-pair$"]["POST"] = generateQrPairInfo;
    server.resource["^/api/qr-pair/cancel$"]["POST"] = cancelQrPair;
    server.resource["^/api/qr-pair$"]["GET"] = getQrPairStatus;
    server.resource["^/api/apps$"]["GET"] = getApps;
    server.resource["^/api/logs$"]["GET"] = getLogs;
    server.resource["^/api/apps$"]["POST"] = saveApp;
    server.resource["^/api/config$"]["GET"] = getConfig;
    server.resource["^/api/config$"]["POST"] = saveConfig;
    server.resource["^/api/gamepad/config$"]["GET"] = getGamepadConfig;
    server.resource["^/api/gamepad/config$"]["POST"] = saveGamepadConfig;
    server.resource["^/api/hdr-enhanced/config$"]["GET"] = getHdrEnhancedConfig;
    server.resource["^/api/hdr-enhanced/config$"]["POST"] = saveHdrEnhancedConfig;
    server.resource["^/api/hdr-enhanced/status$"]["GET"] = getHdrEnhancedStatus;
    server.resource["^/api/hdr-enhanced/components/alkaidlab\\.nvidia_rtx_video/maintenance$"]["POST"] = maintainHdrEnhancedComponent;
    server.resource["^/api/webhook/config$"]["GET"] = getWebhookConfig;
    server.resource["^/api/webhook/config$"]["POST"] = saveWebhookConfig;
    server.resource["^/api/webhook/test$"]["POST"] = testWebhook;
#ifdef _WIN32
    server.resource["^/api/dualsense/config$"]["GET"] = getDualSenseConfig;
    server.resource["^/api/dualsense/config$"]["POST"] = saveDualSenseConfig;
#endif
    server.resource["^/api/configLocale$"]["GET"] = getLocale;
    server.resource["^/api/logout$"]["GET"] = handleLogout;
    server.resource["^/api/logout$"]["POST"] = handleLogout;
    server.resource["^/api/restart$"]["POST"] = restart;
    server.resource["^/api/restart$"]["GET"] = restart;
    server.resource["^/api/boom$"]["GET"] = boom;
    server.resource["^/api/reset-display-device-persistence$"]["POST"] = resetDisplayDevicePersistence;
    server.resource["^/api/microphone/test$"]["POST"] = testMicrophone;
    server.resource["^/api/microphone/status$"]["GET"] = getMicrophoneStatus;
#ifdef _WIN32
    server.resource["^/api/vdd/status$"]["GET"] = getVddStatus;
    server.resource["^/api/vulkan-hdr-bridge$"]["GET"] = getVulkanHdrBridgeStatus;
    server.resource["^/api/vulkan-hdr-bridge/validate$"]["POST"] = validateVulkanHdrBridge;
#endif
    server.resource["^/api/password$"]["POST"] = savePassword;
    server.resource["^/api/apps/([0-9]+)$"]["DELETE"] = deleteApp;
    server.resource["^/api/apps/batch-delete$"]["POST"] = batchDeleteApps;
    server.resource["^/api/clients/unpair-all$"]["POST"] = unpairAll;
    server.resource["^/api/clients/list$"]["GET"] = listClients;
    server.resource["^/api/clients/list$"]["POST"] = saveClients;
    server.resource["^/api/color-profiles$"]["GET"] = listColorProfiles;
    server.resource["^/api/clients/unpair$"]["POST"] = unpair;
    server.resource["^/api/clients/rename$"]["POST"] = renameClient;
    server.resource["^/api/apps/close$"]["POST"] = closeApp;
    server.resource["^/api/covers/upload$"]["POST"] = uploadCover;
    server.resource["^/api/apps/test-menu-cmd$"]["POST"] = testMenuCmd;
    server.resource["^/api/runtime/sessions$"]["GET"] = getRuntimeSessions;
    server.resource["^/api/runtime/hdr$"]["GET"] = getRuntimeHdrStatus;
    server.resource["^/api/runtime/hdr-calibration$"]["GET"] = getRuntimeHdrCalibration;
    server.resource["^/api/runtime/bitrate$"]["GET"] = changeRuntimeBitrate;
    server.resource["^/api/perf/current$"]["GET"] = getPerfCurrent;
    server.resource["^/steam-api/.+$"]["GET"] = proxySteamApi;
    server.resource["^/steam-store/.+$"]["GET"] = proxySteamStore;
    server.resource["^/api/ai/config$"]["GET"] = getAiConfig;
    server.resource["^/api/ai/config$"]["POST"] = saveAiConfigEndpoint;
    server.resource["^/api/ai/models$"]["GET"] = proxyAiModels;
    server.resource["^/api/ai/chat/completions$"]["POST"] = proxyAiChat;
    server.resource["^/api/ai/chat/completions$"]["OPTIONS"] = handleAiCors;
    server.resource["^/api/v1/file-mapping/mappings$"]["GET"] = listFileMappings;
    server.resource["^/api/v1/file-mapping/mappings$"]["POST"] = createFileMapping;
    server.resource["^/api/v1/file-mapping/mappings/([A-Za-z0-9_\\-]{1,64})$"]["PATCH"] = updateFileMapping;
    server.resource["^/api/v1/file-mapping/mappings/([A-Za-z0-9_\\-]{1,64})$"]["DELETE"] = deleteFileMapping;
    server.resource["^/images/sunshine.ico$"]["GET"] = getFaviconImage;
    server.resource["^/images/logo-sunshine-256.png$"]["GET"] = getSunshineLogoImage;
    server.resource["^/images/logo-alkaidlab.png$"]["GET"] = getAlkaidLabLogoImage;
    server.resource["^/images/logo-natpierce.png$"]["GET"] = getNatPierceLogoImage;
    server.resource["^/boxart/.+$"]["GET"] = getBoxArt;

    // Clipboard sync routes are registered in a sibling module so the
    // existing 2700+ line confighttp doesn't grow further. They reuse our
    // basic-auth via the lambda below.
    clipboard_http::register_routes(server,
      [](clipboard_http::resp_https_t resp, clipboard_http::req_https_t req) {
        return authenticate(std::move(resp), std::move(req));
      });
    text_context::http::register_routes(server,
      [](text_context::http::resp_https_t resp, text_context::http::req_https_t req) {
        const auto address = net::addr_to_normalized_string(req->remote_endpoint().address());
        if (net::from_address(address) != net::PC) {
          resp->write(SimpleWeb::StatusCode::client_error_forbidden);
          return false;
        }
        return authenticate(std::move(resp), std::move(req));
      });
    tray_http::auth_fn tray_local_auth = [](tray_http::resp_https_t resp, tray_http::req_https_t req) {
      const auto address = net::addr_to_normalized_string(req->remote_endpoint().address());
      if (config::sunshine.username.empty() && net::from_address(address) == net::PC) {
        return authorize_browser_request(std::move(resp), std::move(req));
      }
      return authenticate(std::move(resp), std::move(req));
    };
    tray_http::register_routes(server, tray_local_auth, tray_local_auth);
    server.resource["^/assets\\/.+$"]["GET"] = getNodeModules;
    server.config.reuse_address = true;
    server.config.address = net::get_bind_address(address_family);
    server.config.port = port_https;
    // Use a small thread pool so that a slow request handler (proxy upstream,
    // file upload, etc.) doesn't block other web UI requests on the same
    // single-threaded io_service.
    server.config.thread_pool_size = 2;
    // Enforce the limit while Simple-Web-Server is buffering the request, before
    // individual handlers parse it. This still leaves headroom for the 10 MiB
    // base64 cover-upload endpoint.
    server.config.max_request_streambuf_size = 16 * 1024 * 1024;

    auto accept_and_run = [&](https_server_t *server) {
      try {
        server->start([](unsigned short port) {
          BOOST_LOG(debug) << "Configuration UI available at [https://localhost:"sv << port << "]"sv;
        });
      }
      catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }
        BOOST_LOG(fatal) << "Couldn't start Configuration HTTPS server on port ["sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
      catch (std::exception &err) {
        BOOST_LOG(fatal) << "Configuration HTTPS server failed to start: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread tcp { accept_and_run, &server };

    // Wait for any event
    shutdown_event->view();

    hdr_enhanced::api::shutdown();
    server.stop();

    tcp.join();
  }
}  // namespace confighttp
