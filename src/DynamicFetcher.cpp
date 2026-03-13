#include "rscraper/DynamicFetcher.hpp"
#include "rscraper/JsonExtractor.hpp"
#include "rscraper/Url.hpp"
#include "rscraper/Utility.hpp"

#include <curl/curl.h>
#include <curl/websockets.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

namespace rscraper {

namespace {

using json = nlohmann::json;

bool ensureCurlInit() {
    static const bool kOk = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    return kOk;
}

bool fileExists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(path), ec);
}

std::vector<std::string> splitPath(std::string_view value) {
    std::vector<std::string> out;
#ifdef _WIN32
    constexpr char sep = ';';
#else
    constexpr char sep = ':';
#endif
    std::size_t start = 0;
    while (start <= value.size()) {
        auto pos = value.find(sep, start);
        if (pos == std::string_view::npos) {
            pos = value.size();
        }
        if (pos > start) {
            out.emplace_back(value.substr(start, pos - start));
        }
        if (pos == value.size()) {
            break;
        }
        start = pos + 1;
    }
    return out;
}

std::string findInPath(const std::string& name) {
    const char* env = std::getenv("PATH");
    if (!env) {
        return "";
    }
    for (const auto& dir : splitPath(env)) {
        auto p = std::filesystem::path(dir) / name;
        if (fileExists(p.string())) {
            return p.string();
        }
#ifdef _WIN32
        if (!p.has_extension()) {
            p.replace_extension(".exe");
            if (fileExists(p.string())) {
                return p.string();
            }
        }
#endif
    }
    return "";
}

std::vector<std::string> browserCandidates() {
    std::vector<std::string> out = {
        "chrome", "chrome.exe", "msedge", "msedge.exe",
        "chromium", "chromium.exe", "google-chrome", "chromium-browser"
    };
#ifdef _WIN32
    out.push_back("C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe");
    out.push_back("C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe");
    out.push_back("C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe");
    out.push_back("C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe");
#endif
    return out;
}

#ifdef _WIN32
std::string quoteArg(const std::string& arg) {
    if (arg.find_first_of(" \t\"") == std::string::npos) {
        return arg;
    }
    std::string out;
    out.push_back('"');
    for (char c : arg) {
        if (c == '"') {
            out += "\\\"";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}
#endif

struct ProcessResult {
    bool success = false;
    bool timeout = false;
    int exitCode = -1;
    std::string stdoutData;
    std::string error;
};

ProcessResult runProcess(const std::string& executable, const std::vector<std::string>& args, int timeoutMs) {
    ProcessResult result;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        result.error = "CreatePipe failed";
        return result;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(STARTUPINFOA);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};
    std::string cmd = quoteArg(executable);
    for (const auto& a : args) {
        cmd.push_back(' ');
        cmd += quoteArg(a);
    }
    std::vector<char> cmdLine(cmd.begin(), cmd.end());
    cmdLine.push_back('\0');

    if (!CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        result.error = "CreateProcess failed";
        return result;
    }
    CloseHandle(writePipe);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 30000);
    std::array<char, 8192> buffer{};
    while (true) {
        DWORD available = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD n = 0;
            ReadFile(readPipe, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &n, nullptr);
            if (n > 0) {
                result.stdoutData.append(buffer.data(), n);
            }
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timeout = true;
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);
    CloseHandle(readPipe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (result.timeout) {
        result.error = "Process timeout";
        return result;
    }
    if (result.exitCode != 0) {
        result.error = "Process exited with code " + std::to_string(result.exitCode);
        return result;
    }
    result.success = true;
#else
    (void)executable;
    (void)args;
    (void)timeoutMs;
    result.error = "not supported on this platform";
#endif
    return result;
}

struct HttpResult {
    bool success = false;
    long status = 0;
    std::string body;
};

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

HttpResult httpRequest(const std::string& url, std::string_view method, int timeoutMs) {
    HttpResult out;
    if (!ensureCurlInit()) {
        return out;
    }
    CURL* easy = curl_easy_init();
    if (!easy) {
        return out;
    }
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &out.body);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs > 0 ? timeoutMs : 3000));
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    if (method == "PUT") {
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "PUT");
    }
    CURLcode rc = curl_easy_perform(easy);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &out.status);
        out.success = (out.status >= 200 && out.status < 300);
    }
    curl_easy_cleanup(easy);
    return out;
}

class WsClient {
public:
    ~WsClient() {
        close();
    }

    bool connect(const std::string& wsUrl, int timeoutMs, std::string& error) {
#ifdef _WIN32
        return connectSocket(wsUrl, timeoutMs, error);
#else
        if (!ensureCurlInit()) {
            error = "curl init failed";
            return false;
        }
        easy_ = curl_easy_init();
        if (!easy_) {
            error = "curl easy init failed";
            return false;
        }
        curl_easy_setopt(easy_, CURLOPT_URL, wsUrl.c_str());
        curl_easy_setopt(easy_, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(easy_, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs > 0 ? timeoutMs : 8000));
        curl_easy_setopt(easy_, CURLOPT_NOSIGNAL, 1L);
        CURLcode rc = curl_easy_perform(easy_);
        if (rc != CURLE_OK) {
            error = curl_easy_strerror(rc);
            return false;
        }
        return true;
#endif
    }

    bool sendText(const std::string& text, std::string& error) {
#ifdef _WIN32
        return sendFrame(0x1, text, error);
#else
        size_t sent = 0;
        CURLcode rc = curl_ws_send(easy_, text.data(), text.size(), &sent, 0, CURLWS_TEXT);
        if (rc != CURLE_OK) {
            error = curl_easy_strerror(rc);
            return false;
        }
        return sent == text.size();
#endif
    }

    bool recvMessage(std::string& out, int timeoutMs, std::string& error) {
#ifdef _WIN32
        out.clear();
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 5000);
        bool startedText = false;
        while (std::chrono::steady_clock::now() < deadline) {
            std::string payload;
            bool fin = false;
            uint8_t opcode = 0;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (!recvFrame(opcode, fin, payload, static_cast<int>(std::max<long long>(1, remaining)), error)) {
                if (error == "ws timeout") {
                    continue;
                }
                return false;
            }

            if (opcode == 0x8) {
                error = "ws closed";
                return false;
            }
            if (opcode == 0x9) {
                (void)sendFrame(0xA, payload, error);
                continue;
            }
            if (opcode == 0xA) {
                continue;
            }
            if (opcode == 0x1) {
                startedText = true;
                out += payload;
                if (fin) {
                    return true;
                }
                continue;
            }
            if (opcode == 0x0 && startedText) {
                out += payload;
                if (fin) {
                    return true;
                }
            }
        }
        error = "ws timeout";
        return false;
#else
        out.clear();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 5000);
        while (std::chrono::steady_clock::now() < deadline) {
            std::array<char, 8192> buf{};
            size_t n = 0;
            const curl_ws_frame* meta = nullptr;
            CURLcode rc = curl_ws_recv(easy_, buf.data(), buf.size(), &n, &meta);
            if (rc == CURLE_AGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (rc != CURLE_OK) {
                error = curl_easy_strerror(rc);
                return false;
            }
            if (n > 0) {
                out.append(buf.data(), n);
            }
            if (meta && meta->bytesleft == 0) {
                return true;
            }
        }
        error = "ws timeout";
        return false;
#endif
    }

private:
    void close() {
#ifdef _WIN32
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
#else
        if (easy_) {
            curl_easy_cleanup(easy_);
            easy_ = nullptr;
        }
#endif
    }

#ifdef _WIN32
    struct ParsedWsUrl {
        std::string host;
        int port = 80;
        std::string path = "/";
    };

    static bool ensureWinsock(std::string& error) {
        static const bool kReady = [] {
            WSADATA data{};
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        if (!kReady) {
            error = "WSAStartup failed";
        }
        return kReady;
    }

    static bool parseWsUrl(const std::string& wsUrl, ParsedWsUrl& out, std::string& error) {
        if (!wsUrl.starts_with("ws://")) {
            error = "Unsupported protocol";
            return false;
        }

        std::string rest = wsUrl.substr(5);
        auto slash = rest.find('/');
        std::string hostPort = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        out.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
        if (hostPort.empty()) {
            error = "Invalid websocket URL";
            return false;
        }

        auto colon = hostPort.rfind(':');
        if (colon != std::string::npos && colon + 1 < hostPort.size()) {
            out.host = hostPort.substr(0, colon);
            try {
                out.port = std::stoi(hostPort.substr(colon + 1));
            } catch (...) {
                error = "Invalid websocket port";
                return false;
            }
        } else {
            out.host = hostPort;
            out.port = 80;
        }

        if (out.host.empty()) {
            error = "Invalid websocket host";
            return false;
        }
        return true;
    }

    static std::string base64Encode(const std::vector<uint8_t>& bytes) {
        static constexpr char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((bytes.size() + 2) / 3) * 4);

        std::size_t i = 0;
        while (i + 2 < bytes.size()) {
            uint32_t chunk = (static_cast<uint32_t>(bytes[i]) << 16) |
                             (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                             static_cast<uint32_t>(bytes[i + 2]);
            out.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
            out.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
            out.push_back(kAlphabet[(chunk >> 6) & 0x3F]);
            out.push_back(kAlphabet[chunk & 0x3F]);
            i += 3;
        }

        if (i < bytes.size()) {
            uint32_t chunk = static_cast<uint32_t>(bytes[i]) << 16;
            out.push_back(kAlphabet[(chunk >> 18) & 0x3F]);
            if (i + 1 < bytes.size()) {
                chunk |= static_cast<uint32_t>(bytes[i + 1]) << 8;
                out.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
                out.push_back(kAlphabet[(chunk >> 6) & 0x3F]);
                out.push_back('=');
            } else {
                out.push_back(kAlphabet[(chunk >> 12) & 0x3F]);
                out.push_back('=');
                out.push_back('=');
            }
        }

        return out;
    }

    static bool waitReadable(SOCKET sock, int timeoutMs) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        const int rc = select(0, &readSet, nullptr, nullptr, &tv);
        return rc > 0;
    }

    static bool sendAll(SOCKET sock, const uint8_t* data, std::size_t len, std::string& error) {
        std::size_t sentTotal = 0;
        while (sentTotal < len) {
            const int sent = send(sock,
                                  reinterpret_cast<const char*>(data + sentTotal),
                                  static_cast<int>(len - sentTotal), 0);
            if (sent <= 0) {
                error = "ws send failed";
                return false;
            }
            sentTotal += static_cast<std::size_t>(sent);
        }
        return true;
    }

    static bool recvExact(SOCKET sock, uint8_t* out, std::size_t len, int timeoutMs, std::string& error) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 5000);
        std::size_t readTotal = 0;
        while (readTotal < len) {
            const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remainingMs <= 0) {
                error = "ws timeout";
                return false;
            }
            if (!waitReadable(sock, static_cast<int>(remainingMs))) {
                error = "ws timeout";
                return false;
            }

            const int n = recv(sock, reinterpret_cast<char*>(out + readTotal),
                               static_cast<int>(len - readTotal), 0);
            if (n <= 0) {
                error = "ws recv failed";
                return false;
            }
            readTotal += static_cast<std::size_t>(n);
        }
        return true;
    }

    bool connectSocket(const std::string& wsUrl, int timeoutMs, std::string& error) {
        close();
        if (!ensureWinsock(error)) {
            return false;
        }

        ParsedWsUrl parsed;
        if (!parseWsUrl(wsUrl, parsed, error)) {
            return false;
        }

        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;

        addrinfo* addrs = nullptr;
        const std::string port = std::to_string(parsed.port);
        if (getaddrinfo(parsed.host.c_str(), port.c_str(), &hints, &addrs) != 0) {
            error = "ws resolve failed";
            return false;
        }

        for (auto* ai = addrs; ai; ai = ai->ai_next) {
            SOCKET candidate = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (candidate == INVALID_SOCKET) {
                continue;
            }
            if (::connect(candidate, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
                socket_ = candidate;
                break;
            }
            closesocket(candidate);
        }
        freeaddrinfo(addrs);

        if (socket_ == INVALID_SOCKET) {
            error = "ws connect failed";
            return false;
        }

        std::vector<uint8_t> randomKey(16);
        std::mt19937 gen(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& b : randomKey) {
            b = static_cast<uint8_t>(dist(gen));
        }
        const std::string secKey = base64Encode(randomKey);

        std::string req =
            "GET " + parsed.path + " HTTP/1.1\r\n"
            "Host: " + parsed.host + ":" + std::to_string(parsed.port) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: " + secKey + "\r\n\r\n";

        if (!sendAll(socket_, reinterpret_cast<const uint8_t*>(req.data()), req.size(), error)) {
            close();
            return false;
        }

        std::string headers;
        std::array<uint8_t, 1024> tmp{};
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 8000);
        while (headers.find("\r\n\r\n") == std::string::npos) {
            const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remainingMs <= 0 || !waitReadable(socket_, static_cast<int>(remainingMs))) {
                close();
                error = "ws handshake timeout";
                return false;
            }
            const int n = recv(socket_, reinterpret_cast<char*>(tmp.data()),
                               static_cast<int>(tmp.size()), 0);
            if (n <= 0) {
                close();
                error = "ws handshake failed";
                return false;
            }
            headers.append(reinterpret_cast<const char*>(tmp.data()), static_cast<std::size_t>(n));
            if (headers.size() > 65536) {
                close();
                error = "ws handshake too large";
                return false;
            }
        }

        if (!headers.starts_with("HTTP/1.1 101") && !headers.starts_with("HTTP/1.0 101")) {
            close();
            error = "ws handshake rejected";
            return false;
        }
        return true;
    }

    bool sendFrame(uint8_t opcode, std::string_view payload, std::string& error) {
        if (socket_ == INVALID_SOCKET) {
            error = "ws not connected";
            return false;
        }

        std::vector<uint8_t> frame;
        frame.reserve(payload.size() + 16);
        frame.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0F))); // FIN + opcode

        const std::size_t len = payload.size();
        if (len <= 125) {
            frame.push_back(static_cast<uint8_t>(0x80 | len));
        } else if (len <= 0xFFFF) {
            frame.push_back(0x80 | 126);
            frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            frame.push_back(static_cast<uint8_t>(len & 0xFF));
        } else {
            frame.push_back(0x80 | 127);
            for (int i = 7; i >= 0; --i) {
                frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
            }
        }

        std::array<uint8_t, 4> mask{};
        static thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& m : mask) {
            m = static_cast<uint8_t>(dist(gen));
            frame.push_back(m);
        }

        frame.resize(frame.size() + len);
        for (std::size_t i = 0; i < len; ++i) {
            frame[frame.size() - len + i] =
                static_cast<uint8_t>(payload[i] ^ static_cast<char>(mask[i % 4]));
        }

        return sendAll(socket_, frame.data(), frame.size(), error);
    }

    bool recvFrame(uint8_t& opcode, bool& fin, std::string& payload, int timeoutMs, std::string& error) {
        opcode = 0;
        fin = false;
        payload.clear();

        std::array<uint8_t, 2> header{};
        if (!recvExact(socket_, header.data(), header.size(), timeoutMs, error)) {
            return false;
        }

        fin = (header[0] & 0x80) != 0;
        opcode = header[0] & 0x0F;
        bool masked = (header[1] & 0x80) != 0;
        uint64_t len = header[1] & 0x7F;

        if (len == 126) {
            std::array<uint8_t, 2> ext{};
            if (!recvExact(socket_, ext.data(), ext.size(), timeoutMs, error)) {
                return false;
            }
            len = (static_cast<uint64_t>(ext[0]) << 8) | static_cast<uint64_t>(ext[1]);
        } else if (len == 127) {
            std::array<uint8_t, 8> ext{};
            if (!recvExact(socket_, ext.data(), ext.size(), timeoutMs, error)) {
                return false;
            }
            len = 0;
            for (uint8_t b : ext) {
                len = (len << 8) | static_cast<uint64_t>(b);
            }
        }

        if (len > 64ULL * 1024ULL * 1024ULL) {
            error = "ws frame too large";
            return false;
        }

        std::array<uint8_t, 4> mask{};
        if (masked) {
            if (!recvExact(socket_, mask.data(), mask.size(), timeoutMs, error)) {
                return false;
            }
        }

        payload.resize(static_cast<std::size_t>(len));
        if (len > 0) {
            if (!recvExact(socket_, reinterpret_cast<uint8_t*>(payload.data()),
                           static_cast<std::size_t>(len), timeoutMs, error)) {
                return false;
            }
            if (masked) {
                for (std::size_t i = 0; i < payload.size(); ++i) {
                    payload[i] = static_cast<char>(
                        static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
                }
            }
        }

        return true;
    }

    SOCKET socket_ = INVALID_SOCKET;
#else
    CURL* easy_ = nullptr;
#endif
};

struct CdpState {
    bool loadFired = false;
    int inFlight = 0;
    std::chrono::steady_clock::time_point navigationStarted = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastActivity = std::chrono::steady_clock::now();
    std::unordered_set<std::string> inFlightRequestIds;
    std::unordered_set<std::string> longLivedRequestIds;
    std::unordered_set<std::string> discovered;
    std::unordered_set<std::string> jsonRequestIds;
    std::unordered_map<std::string, std::string> requestUrlById;
};

constexpr std::size_t kMaxDiscoveredUrls = 4096;
constexpr std::size_t kMaxJsonRequestIds = 512;
constexpr std::size_t kMaxTrackedRequestUrls = 4096;

bool evalValue(const json& response, json& out) {
    if (!response.is_object() || !response.contains("result") || !response["result"].is_object()) {
        return false;
    }
    const auto& r = response["result"];
    if (!r.contains("result") || !r["result"].is_object() || !r["result"].contains("value")) {
        return false;
    }
    out = r["result"]["value"];
    return true;
}

bool containsIcase(std::string_view text, std::string_view needle) {
    return toLowerAscii(text).find(toLowerAscii(needle)) != std::string::npos;
}

std::string resolveAgainst(std::string_view sourceUrl, std::string_view candidate) {
    if (candidate.empty() || candidate.size() > 4096 || UrlCanonicalizer::shouldIgnore(candidate)) {
        return {};
    }

    auto source = Url::parse(sourceUrl);
    if (!source) {
        return std::string(candidate);
    }
    auto resolved = UrlCanonicalizer::resolve(*source, candidate);
    return UrlCanonicalizer::canonicalize(resolved).toStringWithoutFragment();
}

void addDiscoveredUrl(CdpState& state, std::string_view sourceUrl, std::string_view candidate) {
    if (state.discovered.size() >= kMaxDiscoveredUrls) {
        return;
    }
    auto resolved = resolveAgainst(sourceUrl, candidate);
    if (!resolved.empty()) {
        state.discovered.insert(std::move(resolved));
    }
}

bool isLikelyJsonResponse(std::string_view mimeType, std::string_view url, std::string_view resourceType) {
    if (containsIcase(mimeType, "json")) {
        return true;
    }
    if (containsIcase(url, ".json") || containsIcase(url, "/graphql")) {
        return true;
    }
    if ((containsIcase(resourceType, "xhr") || containsIcase(resourceType, "fetch")) &&
        (containsIcase(url, "/api/") || containsIcase(url, "format=json"))) {
        return true;
    }
    return false;
}

std::string getStringOrEmpty(const json& obj, std::string_view key) {
    const std::string keyStr(key);
    if (!obj.is_object() || !obj.contains(keyStr) || !obj[keyStr].is_string()) {
        return {};
    }
    return obj[keyStr].get<std::string>();
}

void handleCdpEvent(const json& msg, CdpState& state) {
    if (!msg.is_object() || !msg.contains("method") || !msg["method"].is_string()) {
        return;
    }

    const std::string method = msg["method"].get<std::string>();
    const json params = msg.contains("params") ? msg["params"] : json::object();
    state.lastActivity = std::chrono::steady_clock::now();

    if (method == "Page.loadEventFired") {
        state.loadFired = true;
        return;
    }

    if (method == "Network.requestWillBeSent") {
        std::string requestId = getStringOrEmpty(params, "requestId");
        std::string requestUrl;
        if (params.contains("request") && params["request"].is_object()) {
            requestUrl = getStringOrEmpty(params["request"], "url");
        }
        if (!requestId.empty() && !state.longLivedRequestIds.contains(requestId)) {
            if (state.inFlightRequestIds.contains(requestId) ||
                state.inFlightRequestIds.size() < kMaxTrackedRequestUrls) {
                state.inFlightRequestIds.insert(requestId);
            }
            state.inFlight = static_cast<int>(state.inFlightRequestIds.size());
        }
        if (!requestId.empty() && !requestUrl.empty()) {
            if (state.requestUrlById.contains(requestId) ||
                state.requestUrlById.size() < kMaxTrackedRequestUrls) {
                state.requestUrlById[requestId] = requestUrl;
            }
        }
        if (!requestUrl.empty()) {
            addDiscoveredUrl(state, requestUrl, requestUrl);
        }
        return;
    }

    if (method == "Network.responseReceived") {
        std::string requestId = getStringOrEmpty(params, "requestId");
        std::string resourceType = getStringOrEmpty(params, "type");
        std::string responseUrl;
        std::string mimeType;

        if (params.contains("response") && params["response"].is_object()) {
            responseUrl = getStringOrEmpty(params["response"], "url");
            mimeType = getStringOrEmpty(params["response"], "mimeType");
        }

        if (!responseUrl.empty()) {
            addDiscoveredUrl(state, responseUrl, responseUrl);
            if (!requestId.empty()) {
                if (state.requestUrlById.contains(requestId) ||
                    state.requestUrlById.size() < kMaxTrackedRequestUrls) {
                    state.requestUrlById[requestId] = responseUrl;
                }
            }
        }

        if (!requestId.empty() && isLikelyJsonResponse(mimeType, responseUrl, resourceType)) {
            if (state.jsonRequestIds.contains(requestId) ||
                state.jsonRequestIds.size() < kMaxJsonRequestIds) {
                state.jsonRequestIds.insert(requestId);
            }
        }

        if (!requestId.empty()) {
            const std::string typeLower = toLowerAscii(resourceType);
            if (typeLower == "websocket" || typeLower == "eventsource" ||
                containsIcase(mimeType, "event-stream")) {
                if (state.longLivedRequestIds.contains(requestId) ||
                    state.longLivedRequestIds.size() < kMaxTrackedRequestUrls) {
                    state.longLivedRequestIds.insert(requestId);
                }
                state.inFlightRequestIds.erase(requestId);
                state.inFlight = static_cast<int>(state.inFlightRequestIds.size());
            }
        }
        return;
    }

    if (method == "Network.loadingFinished" || method == "Network.loadingFailed") {
        std::string requestId = getStringOrEmpty(params, "requestId");
        if (!requestId.empty()) {
            state.inFlightRequestIds.erase(requestId);
            state.longLivedRequestIds.erase(requestId);
            state.requestUrlById.erase(requestId);
        }
        state.inFlight = static_cast<int>(state.inFlightRequestIds.size());
    }
}

bool sendCdp(WsClient& ws, CdpState& state, int& nextId, const std::string& method, const json& params,
             int timeoutMs, json& response, std::string& error) {
    const int id = nextId++;
    if (!ws.sendText(json{{"id", id}, {"method", method}, {"params", params}}.dump(), error)) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 10000);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string msgRaw;
        if (!ws.recvMessage(msgRaw, 200, error)) {
            if (error != "ws timeout") {
                error = "cdp receive failed for " + method + ": " + error;
                return false;
            }
            continue;
        }
        json msg = json::parse(msgRaw, nullptr, false);
        if (!msg.is_object()) {
            continue;
        }
        if (msg.contains("id") && msg["id"].is_number_integer() && msg["id"].get<int>() == id) {
            if (msg.contains("error") && msg["error"].is_object()) {
                std::string cdpMessage = "unknown CDP error";
                if (msg["error"].contains("message") && msg["error"]["message"].is_string()) {
                    cdpMessage = msg["error"]["message"].get<std::string>();
                }
                error = "cdp error for " + method + ": " + cdpMessage;
                return false;
            }
            response = std::move(msg);
            return true;
        }
        handleCdpEvent(msg, state);
    }

    error = "cdp timeout";
    return false;
}

bool waitIdle(WsClient& ws, CdpState& state, int idleMs, int timeoutMs) {
    std::string err;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 10000);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string raw;
        if (!ws.recvMessage(raw, 100, err)) {
            if (err != "ws timeout") {
                return false;
            }
        } else if (!raw.empty()) {
            json msg = json::parse(raw, nullptr, false);
            handleCdpEvent(msg, state);
        }
        const auto now = std::chrono::steady_clock::now();
        const bool loadOrGraceReady =
            state.loadFired || (now - state.navigationStarted) >= std::chrono::milliseconds(1200);
        if (loadOrGraceReady && state.inFlight <= 0 &&
            (now - state.lastActivity) >= std::chrono::milliseconds(std::max(0, idleMs))) {
            return true;
        }
    }
    return false;
}

const char* kScrollJs = R"WM(
(() => {
  try {
    window.scrollTo(0, document.body ? document.body.scrollHeight : 1000000);
    return true;
  } catch (_) {
    return false;
  }
})()
)WM";

const char* kHydrateLazyJs = R"WM(
(() => {
  let changed = 0;
  const setIfMissing = (el, targetAttr, sourceAttr) => {
    const value = (el.getAttribute(sourceAttr) || '').trim();
    if (!value) return;
    if (!el.getAttribute(targetAttr)) {
      el.setAttribute(targetAttr, value);
      changed++;
    }
  };

  for (const img of document.querySelectorAll('img')) {
    setIfMissing(img, 'src', 'data-src');
    setIfMissing(img, 'src', 'data-lazy-src');
    setIfMissing(img, 'src', 'data-original');
    setIfMissing(img, 'srcset', 'data-srcset');
  }
  for (const source of document.querySelectorAll('source')) {
    setIfMissing(source, 'src', 'data-src');
    setIfMissing(source, 'srcset', 'data-srcset');
  }
  for (const el of document.querySelectorAll('[data-bg],[data-background],[data-background-image]')) {
    const value = (el.getAttribute('data-bg') || el.getAttribute('data-background') || el.getAttribute('data-background-image') || '').trim();
    if (!value) continue;
    el.style.backgroundImage = `url("${value}")`;
    changed++;
  }
  return changed;
})()
)WM";

const char* kClickJs = R"WM(
(() => {
  if (!window.__wm) {
    window.__wm = { clickCount: Object.create(null) };
  }
  const textRx = /next|more|load|older|newer|page|strana|dalsi|pokracovat|continue/i;
  const visible = (el) => {
    try {
      const style = getComputedStyle(el);
      if (!style || style.display === 'none' || style.visibility === 'hidden') return false;
      const rect = el.getBoundingClientRect();
      return rect.width > 0 && rect.height > 0;
    } catch (_) {
      return false;
    }
  };

  const selectors = [
    '[data-page]',
    '[data-page-number]',
    '[data-pagenumber]',
    'a[rel=next]',
    '.pagination a',
    '.pager a',
    '.next a',
    'button',
    'a',
    '[role=button]',
    '[class*=next]',
    '[class*=more]',
    '[id*=next]',
    '[id*=more]',
    '[aria-label*=next i]',
    '[aria-label*=more i]',
    '[title*=next i]',
    '[title*=more i]'
  ];

  const nodes = [...new Set(selectors.flatMap((s) => [...document.querySelectorAll(s)]))];
  let clicked = 0;
  for (const el of nodes) {
    if (clicked >= 6) break;
    if (!visible(el)) continue;

    const href = (el.getAttribute('href') || '').trim();
    const rel = (el.getAttribute('rel') || '').toLowerCase();
    const pageAttr = (el.getAttribute('data-page') || el.getAttribute('data-page-number') || el.getAttribute('data-pagenumber') || '').trim();
    const meta = ((el.innerText || el.textContent || '') + ' ' + (el.className || '') + ' ' + (el.id || '') + ' ' + (el.getAttribute('aria-label') || '')).toLowerCase();
    const looksPagination =
      !!pageAttr ||
      href.includes('page=') ||
      href.includes('/page/') ||
      href.includes('p=') ||
      rel.includes('next') ||
      textRx.test(meta);
    if (!looksPagination) continue;

    const key = [el.tagName, href, pageAttr, meta.slice(0, 120)].join('|');
    const repeatable = !href || href === '#' || href.startsWith('javascript:') || textRx.test(meta);
    const maxPerKey = repeatable ? 5 : 1;
    const seen = window.__wm.clickCount[key] || 0;
    if (seen >= maxPerKey) continue;

    try {
      el.click();
      clicked++;
      window.__wm.clickCount[key] = seen + 1;
    } catch (_) {
      // ignore single-node click failures
    }
  }
  return clicked;
})()
)WM";

const char* kInteractionSignalsJs = R"WM(
(() => {
  const selectors = [
    '[data-page]',
    '[data-page-number]',
    '[data-pagenumber]',
    'a[rel=next]',
    '.pagination a',
    '.pager a',
    '.next a',
    '[class*=next]',
    '[class*=more]',
    '[id*=next]',
    '[id*=more]',
    '[aria-label*=next i]',
    '[aria-label*=more i]',
    '[title*=next i]',
    '[title*=more i]',
    '[data-endpoint]',
    '[data-url]',
    '[data-fetch-url]',
    '[data-infinite]',
    '[class*=infinite]',
    '[id*=infinite]'
  ];

  for (const sel of selectors) {
    if (document.querySelector(sel)) {
      return true;
    }
  }
  return false;
})()
)WM";

const char* kHtmlJs = "document.documentElement ? document.documentElement.outerHTML : ''";

const char* kInstallJsonTapJs = R"WM(
(() => {
  if (window.__wm_json_tap_installed) {
    return true;
  }
  window.__wm_json_tap_installed = true;
  window.__wm_json_tap = [];

  const push = (url, body) => {
    if (typeof url !== 'string' || typeof body !== 'string') return;
    if (body.length > 2000000) return;
    window.__wm_json_tap.push({ url, body });
    if (window.__wm_json_tap.length > 200) {
      window.__wm_json_tap.shift();
    }
  };

  if (typeof window.fetch === 'function') {
    const originalFetch = window.fetch.bind(window);
    window.fetch = async (...args) => {
      const response = await originalFetch(...args);
      try {
        const contentType = (response.headers && response.headers.get('content-type')) || '';
        if (typeof contentType === 'string' && contentType.toLowerCase().includes('json')) {
          const text = await response.clone().text();
          const reqUrl = response.url || (args.length > 0 ? String(args[0]) : '');
          push(reqUrl, text);
        }
      } catch (_) {
        // ignore tap failures
      }
      return response;
    };
  }

  if (typeof window.XMLHttpRequest === 'function') {
    const open = XMLHttpRequest.prototype.open;
    const send = XMLHttpRequest.prototype.send;
    XMLHttpRequest.prototype.open = function(method, url, ...rest) {
      this.__wm_url = url;
      return open.call(this, method, url, ...rest);
    };
    XMLHttpRequest.prototype.send = function(...args) {
      this.addEventListener('load', function() {
        try {
          const contentType = (this.getResponseHeader('content-type') || '').toLowerCase();
          if (contentType.includes('json') && typeof this.responseText === 'string') {
            push(this.responseURL || this.__wm_url || '', this.responseText);
          }
        } catch (_) {
          // ignore tap failures
        }
      });
      return send.apply(this, args);
    };
  }

  return true;
})()
)WM";

const char* kReadJsonTapJs = R"WM(
(() => {
  if (!Array.isArray(window.__wm_json_tap)) {
    return [];
  }
  return window.__wm_json_tap.slice(0, 200);
})()
)WM";

const char* kResourcesJs = R"WM(
(() => {
  const set = new Set();
  const add = (value) => {
    if (typeof value === 'string' && value.length > 0 && value.length < 4096) {
      set.add(value);
    }
  };

  for (const el of document.querySelectorAll('[src],[href],[data-src],[data-srcset],[data-href],[data-url],[data-endpoint],[data-fetch-url]')) {
    add(el.getAttribute('src'));
    add(el.getAttribute('href'));
    add(el.getAttribute('data-src'));
    add(el.getAttribute('data-srcset'));
    add(el.getAttribute('data-href'));
    add(el.getAttribute('data-url'));
    add(el.getAttribute('data-endpoint'));
    add(el.getAttribute('data-fetch-url'));
  }

  if (performance && performance.getEntriesByType) {
    for (const resource of performance.getEntriesByType('resource')) {
      if (resource && resource.name) add(resource.name);
    }
  }

  const nextDataNode = document.getElementById('__NEXT_DATA__');
  if (nextDataNode && nextDataNode.textContent) {
    try {
      const root = JSON.parse(nextDataNode.textContent);
      const stack = [root];
      let guard = 0;
      const looksUrl = (value) => /^(https?:)?\/\/|^\/|^\.\.?\//.test(value);
      while (stack.length && guard++ < 20000) {
        const node = stack.pop();
        if (!node) continue;
        if (typeof node === 'string') {
          if (node.length < 4096 && looksUrl(node)) add(node);
          continue;
        }
        if (Array.isArray(node)) {
          for (const child of node) stack.push(child);
          continue;
        }
        if (typeof node === 'object') {
          for (const key of Object.keys(node)) {
            stack.push(node[key]);
          }
        }
      }
    } catch (_) {
      // ignore malformed hydration data
    }
  }

  return Array.from(set);
})()
)WM";

#ifdef _WIN32
bool launchBrowser(const std::string& executable, const std::vector<std::string>& args, PROCESS_INFORMATION& pi) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    HANDLE nullHandle = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullHandle == INVALID_HANDLE_VALUE) {
        return false;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(STARTUPINFOA);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = nullHandle;
    si.hStdError = nullHandle;

    std::string cmd = quoteArg(executable);
    for (const auto& a : args) {
        cmd.push_back(' ');
        cmd += quoteArg(a);
    }
    std::vector<char> cmdLine(cmd.begin(), cmd.end());
    cmdLine.push_back('\0');
    BOOL created = CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(nullHandle);
    return created == TRUE;
}

void killBrowser(PROCESS_INFORMATION& pi) {
    if (pi.hProcess) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        pi.hProcess = nullptr;
    }
    if (pi.hThread) {
        CloseHandle(pi.hThread);
        pi.hThread = nullptr;
    }
}
#endif

class DynamicFetcherProcess final : public IDynamicFetcher {
public:
    explicit DynamicFetcherProcess(DynamicFetcherOptions options) : options_(std::move(options)) {
        auto d = discoverChromiumBrowser(options_);
        browserPath_ = d.browserPath;
        ready_ = d.found;
        if (ready_) {
            auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
            profileDir_ = std::filesystem::temp_directory_path() / ("rscraper-profile-" + std::to_string(ts));
            std::error_code ec;
            std::filesystem::create_directories(profileDir_, ec);
        }
    }

    ~DynamicFetcherProcess() override { close(); }

    DynamicRenderResult renderAndCapture(const std::string& url) override {
        DynamicRenderResult out;
        out.browserPath = browserPath_;
        if (!ready_) {
            out.error = "No Chromium-based browser found";
            return out;
        }

        std::lock_guard<std::mutex> guard(renderMutex_);
#ifdef _WIN32
        auto cdp = renderCdp(url);
        if (cdp.success) {
            spdlog::debug("Dynamic CDP render succeeded for {}", url);
            return cdp;
        }
        if (options_.persistentBrowser) {
            const bool timeoutLikeFailure =
                containsIcase(cdp.error, "timeout") || containsIcase(cdp.error, "timed out");
            if (!timeoutLikeFailure) {
                // Retry once by restarting the persistent browser session.
                stopPersistentBrowser();
                cdp = renderCdp(url);
                if (cdp.success) {
                    spdlog::debug("Dynamic CDP render succeeded after browser restart for {}", url);
                    return cdp;
                }
            } else {
                spdlog::debug("Skipping dynamic browser restart for {} due to timeout-like failure", url);
            }
        }
        if (!cdp.error.empty()) {
            spdlog::debug("Dynamic CDP render failed for {}: {}", url, cdp.error);
        }
#endif
        return renderDumpDom(url);
    }

    bool isReady() const override { return ready_; }

    void close() override {
#ifdef _WIN32
        std::lock_guard<std::mutex> guard(renderMutex_);
        stopPersistentBrowser();
#endif
        if (!profileDir_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(profileDir_, ec);
            profileDir_.clear();
        }
    }

private:
    DynamicRenderResult renderDumpDom(const std::string& url) const {
        DynamicRenderResult out;
        out.browserPath = browserPath_;
        std::vector<std::string> args = {"--headless=new", "--disable-gpu", "--disable-extensions", "--dump-dom",
                                         "--virtual-time-budget=" + std::to_string(options_.waitMs),
                                         "--user-data-dir=" + profileDir_.string()};
        if (!options_.userAgent.empty()) {
            args.push_back("--user-agent=" + options_.userAgent);
        }
        args.push_back(url);
        const int dumpDomTimeoutMs = std::clamp(options_.timeoutMs / 2, 8000, 20000);
        auto p = runProcess(browserPath_, args, dumpDomTimeoutMs);
        if (!p.success) {
            out.error = p.error;
            return out;
        }
        out.html = std::move(p.stdoutData);
        out.success = !out.html.empty();
        if (!out.success) {
            out.error = "Empty rendered HTML";
        }
        return out;
    }

#ifdef _WIN32
    static std::string extractTargetIdFromWsUrl(std::string_view wsUrl) {
        auto slash = wsUrl.rfind('/');
        if (slash == std::string_view::npos || slash + 1 >= wsUrl.size()) {
            return {};
        }
        return std::string(wsUrl.substr(slash + 1));
    }

    int pickDebugPort() const {
        static thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_int_distribution<int> dist(40000, 50000);
        return dist(gen);
    }

    bool waitForDebuggerEndpoint(int port, int timeoutMs) const {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 10000);
        while (std::chrono::steady_clock::now() < deadline) {
            if (httpRequest("http://127.0.0.1:" + std::to_string(port) + "/json/version",
                            "GET", 1000).success) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    bool startPersistentBrowser(std::string& error) {
        stopPersistentBrowser();

        constexpr int kAttempts = 6;
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            const int port = pickDebugPort();
            PROCESS_INFORMATION pi{};
            std::vector<std::string> args = {
                "--headless=new",
                "--disable-gpu",
                "--disable-extensions",
                "--disable-background-networking",
                "--disable-default-apps",
                "--disable-sync",
                "--disable-component-update",
                "--no-first-run",
                "--no-default-browser-check",
                "--metrics-recording-only",
                "--mute-audio",
                "--remote-debugging-address=127.0.0.1",
                "--remote-debugging-port=" + std::to_string(port),
                "--user-data-dir=" + profileDir_.string(),
                "about:blank"
            };
            if (!options_.userAgent.empty()) {
                args.push_back("--user-agent=" + options_.userAgent);
            }

            if (!launchBrowser(browserPath_, args, pi)) {
                error = "Launch failed";
                continue;
            }
            if (!waitForDebuggerEndpoint(port, 10000)) {
                killBrowser(pi);
                error = "CDP endpoint did not start";
                continue;
            }

            persistentBrowserProcess_ = pi;
            persistentBrowserPort_ = port;
            persistentBrowserRunning_ = true;
            spdlog::debug("Started persistent dynamic browser on CDP port {}", persistentBrowserPort_);
            return true;
        }

        if (error.empty()) {
            error = "Could not start dynamic browser";
        }
        return false;
    }

    bool ensurePersistentBrowser(std::string& error) {
        if (persistentBrowserRunning_ && persistentBrowserProcess_.hProcess &&
            WaitForSingleObject(persistentBrowserProcess_.hProcess, 0) == WAIT_TIMEOUT &&
            waitForDebuggerEndpoint(persistentBrowserPort_, 2000)) {
            return true;
        }
        return startPersistentBrowser(error);
    }

    void stopPersistentBrowser() {
        if (persistentBrowserProcess_.hProcess || persistentBrowserProcess_.hThread) {
            killBrowser(persistentBrowserProcess_);
        }
        persistentBrowserRunning_ = false;
        persistentBrowserPort_ = 0;
    }

    void closeCdpTarget(int port, const std::string& targetId) const {
        if (targetId.empty()) {
            return;
        }
        (void)httpRequest(
            "http://127.0.0.1:" + std::to_string(port) + "/json/close/" + targetId,
            "GET", 2000);
    }

    DynamicRenderResult renderCdp(const std::string& url) {
        DynamicRenderResult out;
        out.browserPath = browserPath_;
        int port = 0;
        PROCESS_INFORMATION tempProcess{};
        bool killTempProcess = false;

        if (options_.persistentBrowser) {
            std::string ensureError;
            if (!ensurePersistentBrowser(ensureError)) {
                out.error = ensureError;
                return out;
            }
            port = persistentBrowserPort_;
        } else {
            port = pickDebugPort();
            std::vector<std::string> args = {
                "--headless=new",
                "--disable-gpu",
                "--disable-extensions",
                "--disable-background-networking",
                "--disable-default-apps",
                "--disable-sync",
                "--disable-component-update",
                "--no-first-run",
                "--no-default-browser-check",
                "--metrics-recording-only",
                "--mute-audio",
                "--remote-debugging-address=127.0.0.1",
                "--remote-debugging-port=" + std::to_string(port),
                "--user-data-dir=" + profileDir_.string(),
                "about:blank"
            };
            if (!options_.userAgent.empty()) {
                args.push_back("--user-agent=" + options_.userAgent);
            }
            if (!launchBrowser(browserPath_, args, tempProcess)) {
                out.error = "Launch failed";
                return out;
            }
            killTempProcess = true;
            if (!waitForDebuggerEndpoint(port, 10000)) {
                killBrowser(tempProcess);
                out.error = "CDP endpoint did not start";
                return out;
            }
        }

        auto cleanupProcess = [&]() {
            if (killTempProcess) {
                killBrowser(tempProcess);
            }
        };

        std::string wsUrl;
        std::string targetId;
        auto newTarget = httpRequest("http://127.0.0.1:" + std::to_string(port) + "/json/new?about:blank", "PUT", 2000);
        if (newTarget.success) {
            json target = json::parse(newTarget.body, nullptr, false);
            if (target.is_object()) {
                if (target.contains("webSocketDebuggerUrl") &&
                    target["webSocketDebuggerUrl"].is_string()) {
                    wsUrl = target["webSocketDebuggerUrl"].get<std::string>();
                }
                if (target.contains("id") && target["id"].is_string()) {
                    targetId = target["id"].get<std::string>();
                }
            }
        }

        if (wsUrl.empty()) {
            auto list = httpRequest("http://127.0.0.1:" + std::to_string(port) + "/json/list", "GET", 2000);
            if (!list.success) {
                cleanupProcess();
                out.error = "CDP list failed";
                return out;
            }
            json listJson = json::parse(list.body, nullptr, false);
            if (listJson.is_array()) {
                for (const auto& item : listJson) {
                    if (!item.is_object()) {
                        continue;
                    }
                    if (!item.contains("webSocketDebuggerUrl") || !item["webSocketDebuggerUrl"].is_string()) {
                        continue;
                    }
                    if (item.contains("type") && item["type"].is_string() &&
                        item["type"].get<std::string>() == "page") {
                        wsUrl = item["webSocketDebuggerUrl"].get<std::string>();
                        if (item.contains("id") && item["id"].is_string()) {
                            targetId = item["id"].get<std::string>();
                        }
                        break;
                    }
                    if (wsUrl.empty()) {
                        wsUrl = item["webSocketDebuggerUrl"].get<std::string>();
                        if (item.contains("id") && item["id"].is_string()) {
                            targetId = item["id"].get<std::string>();
                        }
                    }
                }
            }
        }
        if (wsUrl.empty()) {
            cleanupProcess();
            out.error = "No CDP target";
            return out;
        }
        if (targetId.empty()) {
            targetId = extractTargetIdFromWsUrl(wsUrl);
        }
        auto cleanupTarget = [&]() { closeCdpTarget(port, targetId); };

        WsClient ws;
        std::string wsError;
        if (!ws.connect(wsUrl, 8000, wsError)) {
            cleanupTarget();
            cleanupProcess();
            out.error = wsError;
            return out;
        }

        CdpState state;
        int nextId = 1;
        json response;
        std::string err;
        if (!sendCdp(ws, state, nextId, "Page.enable", json::object(), 5000, response, err) ||
            !sendCdp(ws, state, nextId, "Runtime.enable", json::object(), 5000, response, err) ||
            !sendCdp(ws, state, nextId, "DOM.enable", json::object(), 5000, response, err) ||
            !sendCdp(ws, state, nextId, "Network.enable", json::object(), 5000, response, err)) {
            cleanupTarget();
            cleanupProcess();
            out.error = "CDP init failed: " + err;
            return out;
        }

        if (options_.blockHeavyResources) {
            const json blocked = json::array({
                "*.woff", "*.woff2", "*.ttf", "*.otf", "*.eot",
                "*.mp4", "*.webm", "*.mp3", "*.wav", "*.ogg",
                "*googletagmanager*", "*google-analytics*", "*doubleclick.net*",
                "*facebook.net*", "*clarity.ms*", "*hotjar*"
            });
            (void)sendCdp(ws, state, nextId, "Network.setBlockedURLs",
                          {{"urls", blocked}}, 3000, response, err);
        }

        if (!sendCdp(ws, state, nextId, "Page.addScriptToEvaluateOnNewDocument",
                     {{"source", kInstallJsonTapJs}}, 5000, response, err)) {
            cleanupTarget();
            cleanupProcess();
            out.error = "CDP init failed: " + err;
            return out;
        }

        state.loadFired = false;
        state.inFlight = 0;
        state.inFlightRequestIds.clear();
        state.longLivedRequestIds.clear();
        state.requestUrlById.clear();
        state.navigationStarted = std::chrono::steady_clock::now();
        state.lastActivity = state.navigationStarted;

        if (!sendCdp(ws, state, nextId, "Page.navigate", {{"url", url}}, 10000, response, err)) {
            cleanupTarget();
            cleanupProcess();
            out.error = "CDP init failed: " + err;
            return out;
        }

        const int initialIdleTimeoutMs = std::clamp(options_.waitMs + 2000, 2500, 7000);
        (void)waitIdle(ws, state, options_.idleMs, initialIdleTimeoutMs);
        const int settleTimeoutMs = std::clamp(options_.waitMs / 2, 500, 2200);
        const int settleAfterClickMs = std::clamp(options_.waitMs / 2 + 300, 800, 2200);
        spdlog::debug("CDP initial network capture: {} URLs, {} JSON request ids",
                      state.discovered.size(), state.jsonRequestIds.size());
        (void)sendCdp(ws, state, nextId, "Runtime.evaluate",
                      {{"expression", kHydrateLazyJs}, {"returnByValue", true}, {"awaitPromise", true}},
                      2500, response, err);
        (void)waitIdle(ws, state, options_.idleMs, settleTimeoutMs);

        bool hasInteractionSignals = false;
        if (options_.enableInteractions) {
            if (sendCdp(ws, state, nextId, "Runtime.evaluate",
                        {{"expression", kInteractionSignalsJs}, {"returnByValue", true}, {"awaitPromise", true}},
                        2500, response, err)) {
                json interactionSignalsValue;
                if (evalValue(response, interactionSignalsValue) && interactionSignalsValue.is_boolean()) {
                    hasInteractionSignals = interactionSignalsValue.get<bool>();
                }
            }
            if (!hasInteractionSignals) {
                for (const auto& discoveredUrl : state.discovered) {
                    if (containsIcase(discoveredUrl, "page=") ||
                        containsIcase(discoveredUrl, "cursor=") ||
                        containsIcase(discoveredUrl, "/page/") ||
                        containsIcase(discoveredUrl, "load-more") ||
                        containsIcase(discoveredUrl, "infinite")) {
                        hasInteractionSignals = true;
                        break;
                    }
                }
            }
        }

        int interactionBudget = std::clamp(options_.interactionSteps, 0, 6);
        if (options_.enableInteractions) {
            if (!hasInteractionSignals) {
                interactionBudget = std::min(interactionBudget, 1);
            } else if (interactionBudget > 0) {
                interactionBudget = std::max(interactionBudget, 2);
            }
        }

        int stagnantInteractionRounds = 0;
        std::size_t discoveredBeforeRound = state.discovered.size();
        for (int i = 0; options_.enableInteractions && i < interactionBudget; ++i) {
            (void)sendCdp(ws, state, nextId, "Runtime.evaluate",
                          {{"expression", kScrollJs}, {"returnByValue", true}, {"awaitPromise", true}},
                          2500, response, err);
            (void)sendCdp(ws, state, nextId, "Runtime.evaluate",
                          {{"expression", kHydrateLazyJs}, {"returnByValue", true}, {"awaitPromise", true}},
                          2500, response, err);
            (void)waitIdle(ws, state, options_.idleMs, settleTimeoutMs);
            if (!sendCdp(ws, state, nextId, "Runtime.evaluate",
                         {{"expression", kClickJs}, {"returnByValue", true}, {"awaitPromise", true}},
                         4000, response, err)) {
                break;
            }
            json clicked;
            int count = 0;
            if (evalValue(response, clicked) && clicked.is_number_integer()) {
                count = clicked.get<int>();
            }
            if (count <= 0) {
                break;
            }
            (void)waitIdle(ws, state, options_.idleMs, settleAfterClickMs);

            const std::size_t growth = state.discovered.size() > discoveredBeforeRound
                                           ? state.discovered.size() - discoveredBeforeRound
                                           : 0;
            if (growth < 3) {
                ++stagnantInteractionRounds;
            } else {
                stagnantInteractionRounds = 0;
            }
            discoveredBeforeRound = state.discovered.size();
            if (stagnantInteractionRounds >= 2) {
                spdlog::debug("Stopping dynamic interactions early due to stagnant discovery");
                break;
            }
        }

        (void)sendCdp(ws, state, nextId, "Runtime.evaluate",
                      {{"expression", "(() => { try { if (window.stop) window.stop(); return true; } catch (_) { return false; } })()"},
                       {"returnByValue", true}, {"awaitPromise", true}},
                      1200, response, err);
        (void)sendCdp(ws, state, nextId, "Runtime.evaluate",
                      {{"expression", kHydrateLazyJs}, {"returnByValue", true}, {"awaitPromise", true}},
                      2500, response, err);
        (void)waitIdle(ws, state, options_.idleMs, settleTimeoutMs);
        (void)sendCdp(ws, state, nextId, "Network.disable", json::object(), 1200, response, err);

        auto tryCaptureHtml = [&](int timeoutMs) {
            json localResponse;
            std::string localError;
            if (!sendCdp(ws, state, nextId, "Runtime.evaluate",
                         {{"expression", kHtmlJs}, {"returnByValue", true}, {"awaitPromise", true}},
                         timeoutMs, localResponse, localError)) {
                err = localError;
                return false;
            }
            json htmlValue;
            if (!evalValue(localResponse, htmlValue) || !htmlValue.is_string()) {
                err = "invalid HTML evaluate response";
                return false;
            }
            out.html = htmlValue.get<std::string>();
            if (out.html.empty()) {
                err = "empty HTML evaluate response";
                return false;
            }
            return true;
        };

        auto tryCaptureHtmlFromDom = [&]() {
            json domResponse;
            std::string domError;
            if (!sendCdp(ws, state, nextId, "DOM.getDocument",
                         {{"depth", 1}}, 6000, domResponse, domError)) {
                err = "DOM.getDocument failed: " + domError;
                return false;
            }
            if (!domResponse.is_object() || !domResponse.contains("result") ||
                !domResponse["result"].is_object() ||
                !domResponse["result"].contains("root") ||
                !domResponse["result"]["root"].is_object() ||
                !domResponse["result"]["root"].contains("nodeId") ||
                !domResponse["result"]["root"]["nodeId"].is_number_integer()) {
                err = "DOM.getDocument returned invalid root";
                return false;
            }

            const int rootNodeId = domResponse["result"]["root"]["nodeId"].get<int>();
            if (!sendCdp(ws, state, nextId, "DOM.getOuterHTML",
                         {{"nodeId", rootNodeId}}, 10000, domResponse, domError)) {
                err = "DOM.getOuterHTML failed: " + domError;
                return false;
            }
            if (!domResponse.is_object() || !domResponse.contains("result") ||
                !domResponse["result"].is_object() ||
                !domResponse["result"].contains("outerHTML") ||
                !domResponse["result"]["outerHTML"].is_string()) {
                err = "DOM.getOuterHTML returned invalid payload";
                return false;
            }

            out.html = domResponse["result"]["outerHTML"].get<std::string>();
            if (out.html.empty()) {
                err = "DOM.getOuterHTML returned empty HTML";
                return false;
            }
            return true;
        };

        if (!tryCaptureHtml(6000)) {
            (void)waitIdle(ws, state, std::min(options_.idleMs, 400), 1800);
            (void)sendCdp(ws, state, nextId, "Runtime.evaluate",
                          {{"expression", kHydrateLazyJs}, {"returnByValue", true}, {"awaitPromise", true}},
                          1800, response, err);
            if (!tryCaptureHtml(10000) && !tryCaptureHtmlFromDom()) {
                cleanupTarget();
                cleanupProcess();
                out.error = "HTML capture failed: " + err;
                return out;
            }
        }

        if (sendCdp(ws, state, nextId, "Runtime.evaluate",
                    {{"expression", kResourcesJs}, {"returnByValue", true}, {"awaitPromise", true}},
                    4500, response, err)) {
            json resources;
            if (evalValue(response, resources) && resources.is_array()) {
                for (const auto& item : resources) {
                    if (item.is_string()) {
                        addDiscoveredUrl(state, url, item.get<std::string>());
                    }
                }
            }
        }

        auto collectJsonBody = [&](std::string_view sourceUrl, std::string_view body) {
            auto extracted = JsonExtractor::extract(body, sourceUrl, options_.ajaxMaxPages);
            for (const auto& extractedUrl : extracted.urls) {
                addDiscoveredUrl(state, sourceUrl, extractedUrl);
            }
            for (const auto& extractedUrl : extracted.paginationUrls) {
                addDiscoveredUrl(state, sourceUrl, extractedUrl);
            }
        };

        int tappedJsonBodies = 0;
        // Prefer in-page fetch/XHR body tap because it also covers POST-only JSON APIs.
        if (sendCdp(ws, state, nextId, "Runtime.evaluate",
                    {{"expression", kReadJsonTapJs}, {"returnByValue", true}, {"awaitPromise", true}},
                    4500, response, err)) {
            json tappedBodies;
            if (evalValue(response, tappedBodies) && tappedBodies.is_array()) {
                for (const auto& entry : tappedBodies) {
                    if (!entry.is_object()) {
                        continue;
                    }
                    std::string sourceUrl = getStringOrEmpty(entry, "url");
                    std::string body = getStringOrEmpty(entry, "body");
                    if (sourceUrl.empty() || body.empty()) {
                        continue;
                    }
                    collectJsonBody(sourceUrl, body);
                    ++tappedJsonBodies;
                }
            }
        }

        // Pull JSON response bodies directly from CDP so POST-only APIs still yield assets/pages.
        int maxJsonBodies = std::clamp(options_.ajaxMaxPages / 2, 16, 96);
        if (hasInteractionSignals) {
            maxJsonBodies = std::min(maxJsonBodies + 24, 128);
        }
        if (tappedJsonBodies > 0 && !hasInteractionSignals) {
            maxJsonBodies = std::min(maxJsonBodies, 24);
        }

        std::vector<std::pair<int, std::string>> prioritizedRequestIds;
        prioritizedRequestIds.reserve(state.jsonRequestIds.size());
        for (const auto& requestId : state.jsonRequestIds) {
            std::string requestUrl = url;
            auto it = state.requestUrlById.find(requestId);
            if (it != state.requestUrlById.end() && !it->second.empty()) {
                requestUrl = it->second;
            }

            int score = 0;
            if (containsIcase(requestUrl, "/api/") || containsIcase(requestUrl, "graphql")) {
                score += 5;
            }
            if (containsIcase(requestUrl, "page=") || containsIcase(requestUrl, "cursor=") ||
                containsIcase(requestUrl, "offset=") || containsIcase(requestUrl, "/page/")) {
                score += 4;
            }
            if (containsIcase(requestUrl, ".json") || containsIcase(requestUrl, "format=json")) {
                score += 2;
            }

            prioritizedRequestIds.emplace_back(score, requestId);
        }
        std::sort(prioritizedRequestIds.begin(), prioritizedRequestIds.end(),
                  [](const auto& lhs, const auto& rhs) {
                      if (lhs.first != rhs.first) {
                          return lhs.first > rhs.first;
                      }
                      return lhs.second < rhs.second;
                  });

        int processedJsonBodies = 0;
        for (const auto& [score, requestId] : prioritizedRequestIds) {
            (void)score;
            if (processedJsonBodies >= maxJsonBodies) {
                break;
            }
            if (!sendCdp(ws, state, nextId, "Network.getResponseBody",
                         {{"requestId", requestId}}, 2200, response, err)) {
                continue;
            }
            if (!response.is_object() || !response.contains("result") || !response["result"].is_object()) {
                continue;
            }
            const json& resultObj = response["result"];
            if (!resultObj.contains("body") || !resultObj["body"].is_string()) {
                continue;
            }
            if (resultObj.contains("base64Encoded") &&
                resultObj["base64Encoded"].is_boolean() &&
                resultObj["base64Encoded"].get<bool>()) {
                continue;
            }

            std::string sourceUrl = url;
            auto it = state.requestUrlById.find(requestId);
            if (it != state.requestUrlById.end() && !it->second.empty()) {
                sourceUrl = it->second;
            }

            collectJsonBody(sourceUrl, resultObj["body"].get<std::string>());
            ++processedJsonBodies;
        }

        out.discoveredUrls.assign(state.discovered.begin(), state.discovered.end());
        std::sort(out.discoveredUrls.begin(), out.discoveredUrls.end());
        spdlog::debug("CDP render finished for {} with {} discovered URLs", url, out.discoveredUrls.size());
        cleanupTarget();
        cleanupProcess();
        out.success = !out.html.empty();
        if (!out.success) {
            out.error = "CDP empty HTML";
        }
        return out;
    }
#endif

    DynamicFetcherOptions options_;
    std::string browserPath_;
    bool ready_ = false;
    std::filesystem::path profileDir_;
    mutable std::mutex renderMutex_;
#ifdef _WIN32
    PROCESS_INFORMATION persistentBrowserProcess_{};
    int persistentBrowserPort_ = 0;
    bool persistentBrowserRunning_ = false;
#endif
};

} // namespace

BrowserDiscoveryResult discoverChromiumBrowser(const DynamicFetcherOptions& options) {
    BrowserDiscoveryResult out;
    auto tryOne = [&out](const std::string& candidate) {
        if (candidate.empty()) {
            return false;
        }
        out.checkedCandidates.push_back(candidate);
        if (fileExists(candidate)) {
            out.found = true;
            out.browserPath = candidate;
            return true;
        }
        std::filesystem::path p(candidate);
        if (p.has_parent_path()) {
            return false;
        }
        auto fromPath = findInPath(candidate);
        if (!fromPath.empty()) {
            out.found = true;
            out.browserPath = fromPath;
            return true;
        }
        return false;
    };
    if (tryOne(options.browserPath)) {
        return out;
    }
    for (const auto& c : browserCandidates()) {
        if (tryOne(c)) {
            return out;
        }
    }
    return out;
}

std::unique_ptr<IDynamicFetcher> createDynamicFetcher(const DynamicFetcherOptions& options) {
    return std::make_unique<DynamicFetcherProcess>(options);
}

} // namespace rscraper
