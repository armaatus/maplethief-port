// UpdateFetcher over the system's HTTP (#199, ADR 0137): WinHTTP on Windows, libcurl on Linux
// when the system has it (loaded at run time, so it is no build or link dependency). Apple's is
// update_fetch_apple.mm. Elsewhere there is none: make_platform_update_fetcher returns nullptr
// and the update check does not run.
#include "update_check.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <winhttp.h>
// clang-format on

namespace maplethief::frontend {

namespace {

std::wstring widen(std::string_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

struct InternetHandle {
    HINTERNET handle = nullptr;
    explicit InternetHandle(HINTERNET h) : handle(h) {}
    InternetHandle(const InternetHandle &) = delete;
    InternetHandle &operator=(const InternetHandle &) = delete;
    ~InternetHandle() {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
};

std::optional<std::string> fail(std::string *why, const char *step) {
    if (why != nullptr) {
        *why = std::string(step) + " failed (error " + std::to_string(GetLastError()) + ")";
    }
    return std::nullopt;
}

class WinHttpFetcher final : public UpdateFetcher {
  public:
    std::optional<std::string> get(const std::string &url, std::string *why) override {
        constexpr std::string_view scheme = "https://";
        if (url.rfind(scheme, 0) != 0) {
            if (why != nullptr) {
                *why = "not an https address";
            }
            return std::nullopt;
        }
        const std::string_view rest = std::string_view(url).substr(scheme.size());
        const std::size_t slash = rest.find('/');
        const std::wstring host = widen(rest.substr(0, slash));
        const std::wstring path =
            slash == std::string_view::npos ? std::wstring(L"/") : widen(rest.substr(slash));
        const std::wstring agent = widen(update_user_agent());

        const InternetHandle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                                 0));
        if (session.handle == nullptr) {
            return fail(why, "WinHttpOpen");
        }
        const int timeout_ms = kUpdateTimeoutSeconds * 1000;
        if (!WinHttpSetTimeouts(session.handle, timeout_ms, timeout_ms, timeout_ms, timeout_ms)) {
            return fail(why, "WinHttpSetTimeouts");
        }
        const InternetHandle connection(
            WinHttpConnect(session.handle, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (connection.handle == nullptr) {
            return fail(why, "WinHttpConnect");
        }
        const InternetHandle request(
            WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (request.handle == nullptr) {
            return fail(why, "WinHttpOpenRequest");
        }
        if (!WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            return fail(why, "WinHttpSendRequest");
        }
        if (!WinHttpReceiveResponse(request.handle, nullptr)) {
            return fail(why, "WinHttpReceiveResponse");
        }
        DWORD status = 0;
        DWORD status_size = sizeof status;
        if (!WinHttpQueryHeaders(request.handle,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            return fail(why, "WinHttpQueryHeaders");
        }
        if (status < 200 || status >= 300) {
            if (why != nullptr) {
                *why = "HTTP status " + std::to_string(status);
            }
            return std::nullopt;
        }
        std::string body;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.handle, &available)) {
                return fail(why, "WinHttpQueryDataAvailable");
            }
            if (available == 0) {
                return body;
            }
            if (body.size() + available > kUpdateMaxResponseBytes) {
                if (why != nullptr) {
                    *why = "the response is too long";
                }
                return std::nullopt;
            }
            const std::size_t at = body.size();
            body.resize(at + available);
            DWORD got = 0;
            if (!WinHttpReadData(request.handle, body.data() + at, available, &got)) {
                return fail(why, "WinHttpReadData");
            }
            body.resize(at + got);
        }
    }
};

} // namespace

std::unique_ptr<UpdateFetcher> make_platform_update_fetcher() {
    return std::make_unique<WinHttpFetcher>();
}

} // namespace maplethief::frontend

#elif defined(__linux__)

#include <SDL3/SDL.h>

#include <utility>

namespace maplethief::frontend {

namespace {

// libcurl's ABI (soname 4, unchanged since 2006): its option numbers, written out so that the
// build needs neither its headers nor its library.
constexpr int kCurlOptWriteData = 10001;
constexpr int kCurlOptUrl = 10002;
constexpr int kCurlOptUserAgent = 10018;
constexpr int kCurlOptTimeout = 13;
constexpr int kCurlOptFailOnError = 45;
constexpr int kCurlOptFollowLocation = 52;
constexpr int kCurlOptNoSignal = 99;
constexpr int kCurlOptWriteFunction = 20011;

using CurlInit = void *(*)();
using CurlSetopt = int (*)(void *, int, ...);
using CurlPerform = int (*)(void *);
using CurlCleanup = void (*)(void *);
using CurlStrerror = const char *(*)(int);

struct Sink {
    std::string body;
    bool too_long = false;
};

std::size_t curl_write(const char *data, std::size_t size, std::size_t count, void *user) {
    auto *sink = static_cast<Sink *>(user);
    const std::size_t bytes = size * count;
    if (sink->body.size() + bytes > kUpdateMaxResponseBytes) {
        sink->too_long = true;
        return 0; // libcurl stops with a write error
    }
    sink->body.append(data, bytes);
    return bytes;
}

class CurlFetcher final : public UpdateFetcher {
  public:
    static std::unique_ptr<CurlFetcher> load() {
        SDL_SharedObject *lib = SDL_LoadObject("libcurl.so.4");
        if (lib == nullptr) {
            return nullptr;
        }
        auto out = std::unique_ptr<CurlFetcher>(new CurlFetcher());
        out->init_ = reinterpret_cast<CurlInit>(SDL_LoadFunction(lib, "curl_easy_init"));
        out->setopt_ = reinterpret_cast<CurlSetopt>(SDL_LoadFunction(lib, "curl_easy_setopt"));
        out->perform_ = reinterpret_cast<CurlPerform>(SDL_LoadFunction(lib, "curl_easy_perform"));
        out->cleanup_ = reinterpret_cast<CurlCleanup>(SDL_LoadFunction(lib, "curl_easy_cleanup"));
        out->strerror_ =
            reinterpret_cast<CurlStrerror>(SDL_LoadFunction(lib, "curl_easy_strerror"));
        if (out->init_ == nullptr || out->setopt_ == nullptr || out->perform_ == nullptr ||
            out->cleanup_ == nullptr || out->strerror_ == nullptr) {
            SDL_UnloadObject(lib);
            return nullptr;
        }
        return out; // the library stays loaded for the process's life
    }

    std::optional<std::string> get(const std::string &url, std::string *why) override {
        void *curl = init_();
        if (curl == nullptr) {
            if (why != nullptr) {
                *why = "curl_easy_init failed";
            }
            return std::nullopt;
        }
        const std::string agent = update_user_agent();
        Sink sink;
        setopt_(curl, kCurlOptUrl, url.c_str());
        setopt_(curl, kCurlOptUserAgent, agent.c_str());
        setopt_(curl, kCurlOptWriteFunction, &curl_write);
        setopt_(curl, kCurlOptWriteData, &sink);
        setopt_(curl, kCurlOptFollowLocation, 1L);
        setopt_(curl, kCurlOptFailOnError, 1L);
        setopt_(curl, kCurlOptNoSignal, 1L); // a background thread: no SIGALRM
        setopt_(curl, kCurlOptTimeout, static_cast<long>(kUpdateTimeoutSeconds));
        const int code = perform_(curl);
        cleanup_(curl);
        if (code != 0) {
            if (why != nullptr) {
                *why = sink.too_long ? std::string("the response is too long")
                                     : std::string(strerror_(code));
            }
            return std::nullopt;
        }
        return std::optional<std::string>(std::move(sink.body));
    }

  private:
    CurlFetcher() = default;
    CurlInit init_ = nullptr;
    CurlSetopt setopt_ = nullptr;
    CurlPerform perform_ = nullptr;
    CurlCleanup cleanup_ = nullptr;
    CurlStrerror strerror_ = nullptr;
};

} // namespace

std::unique_ptr<UpdateFetcher> make_platform_update_fetcher() { return CurlFetcher::load(); }

} // namespace maplethief::frontend

#elif !defined(__APPLE__)

namespace maplethief::frontend {

std::unique_ptr<UpdateFetcher> make_platform_update_fetcher() { return nullptr; }

} // namespace maplethief::frontend

#endif
