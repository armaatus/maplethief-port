// The opt-in update notice (#199, ADR 0137): a player on a downloaded build learns that a newer
// port release exists, if they chose to. The game itself is offline (ADR 0034); this is the
// port's only network use. It never downloads anything.
//
//   update_check = off | on   (settings.h: Other, off by default, not in the presets)
//
// When it runs: update_check_wanted (the setting on, the preset not Original, an interactive
// run). Otherwise nothing happens at all: no state file is read, no fetcher is made, no thread
// starts. The first-launch picker asks once (preset_picker.h); the settings menu changes it.
//
// What it does, at most once a day (update_check_due): one GET of kUpdateApiUrl on a background
// thread, through one interface (UpdateFetcher) over the system's HTTP: NSURLSession on macOS and
// iOS (update_fetch_apple.mm), WinHTTP on Windows, libcurl on Linux when the system has it,
// loaded at run time (update_fetch.cpp). Without one there is no check. The tag of the latest
// release is compared with the running version as SemVer; drafts and pre-releases are skipped.
//
// The response is untrusted: only tag_name, draft and prerelease are read, the tag must be SemVer
// and the release page's address is built from it, never taken from the response.
//
// State: update_check.txt in the data folder, the time of the last check (written before the
// request, so a failed check counts too) and the latest tag seen, so the notice survives a
// restart within the day. A file that cannot be read counts as never checked.
//
// Failures are silent apart from one log line. The notice shows on the title screen's corner
// and in the settings menu (game.cpp), never during play; each opens the release page
// (SDL_OpenURL) when the player chooses it.
//
// Threads: start() and the getters on the frontend's thread; the check on its own detached
// thread, which shares only UpdateCheck's reference-counted state, so quitting never waits.
#ifndef MAPLETHIEF_PLATFORM_UPDATE_CHECK_H
#define MAPLETHIEF_PLATFORM_UPDATE_CHECK_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "settings.h"

namespace maplethief::frontend {

inline constexpr const char *kUpdateApiUrl =
    "https://api.github.com/repos/armaatus/maplethief-port/releases/latest";
// The release page is this plus the tag; the notice names the repository only, to fit a line.
inline constexpr const char *kUpdateReleasePage =
    "https://github.com/armaatus/maplethief-port/releases/tag/";
inline constexpr const char *kUpdateRepoShort = "github.com/armaatus/maplethief-port";
inline constexpr const char *kUpdateStateFile = "update_check.txt";
inline constexpr std::int64_t kUpdateIntervalSeconds = 24 * 60 * 60;
// The most of a response a fetcher keeps, and how long it waits for one.
inline constexpr std::size_t kUpdateMaxResponseBytes = 4 * 1024 * 1024;
inline constexpr int kUpdateTimeoutSeconds = 15;

// A version, SemVer 2.0.0. A leading 'v' is allowed (the tags are vX.Y.Z); leading zeros and
// empty identifiers are not.
struct SemVer {
    std::uint64_t major = 0, minor = 0, patch = 0;
    std::vector<std::string> pre; // the pre-release identifiers ("rc", "1"); empty: a release
    std::string build;            // the build metadata; ignored by the comparison
};
std::optional<SemVer> parse_semver(std::string_view text);
// Negative, zero or positive as a is older than, as old as or newer than b, by SemVer's
// precedence: a pre-release is older than its release, build metadata does not count.
int compare_semver(const SemVer &a, const SemVer &b);

// What the check reads of GitHub's "latest release" response.
struct ReleaseInfo {
    std::string tag;
    bool prerelease = false;
    bool draft = false;
};
// The object's own tag_name (required), prerelease and draft; nested objects (the author, the
// assets) are stepped over, whatever keys they hold. std::nullopt if it is not valid JSON or
// tag_name is missing.
std::optional<ReleaseInfo> parse_release_json(std::string_view body);
// The tag, if `release` is a finished release (no draft, no pre-release) newer than `running`.
std::optional<std::string> newer_release(const ReleaseInfo &release, std::string_view running);

// Whether a check is due at `now` (seconds since the epoch) after one at `last` (none: yes). A
// `last` in the future (a clock set back) is due too, so it cannot stop the checks for good.
bool update_check_due(std::int64_t now, std::optional<std::int64_t> last);
// The rule of the header above. `interactive`: desktop.h's is_interactive_run.
bool update_check_wanted(const Settings &settings, bool interactive);

// update_check.txt.
struct UpdateState {
    std::optional<std::int64_t> last_check;
    std::string latest_tag; // "" until one was seen
};
// Never fails: anything it cannot read counts as absent.
UpdateState parse_update_state(std::string_view text);
std::string serialise_update_state(const UpdateState &state);

// The seam (CI never goes online). One GET of `url`: the body of a 2xx response, or std::nullopt
// with *why set (for the log). Called on the check's thread, at most once per check.
class UpdateFetcher {
  public:
    virtual ~UpdateFetcher() = default;
    virtual std::optional<std::string> get(const std::string &url, std::string *why) = 0;
};
using UpdateFetcherFactory = std::function<std::unique_ptr<UpdateFetcher>()>;
// The system's HTTP, or nullptr where there is none (Linux without libcurl). Makes no request.
std::unique_ptr<UpdateFetcher> make_platform_update_fetcher();
// The integer options libcurl's fetcher (Linux) sets on each handle besides the URL, the agent
// and the writer, as (option number, value): testable on every system.
std::vector<std::pair<int, long>> curl_long_options();
// "MapleThief/<version>": every fetcher's User-Agent (GitHub's API requires one).
std::string update_user_agent();

struct UpdateCheckConfig {
    bool wanted = false;               // update_check_wanted
    std::string state_path;            // update_check.txt in the data folder
    std::string running_version;       // MT_VERSION
    std::int64_t now = 0;              // seconds since the epoch
    UpdateFetcherFactory make_fetcher; // make_platform_update_fetcher; only called if one runs
    std::function<void(const std::string &)> log; // the log's lines; null: SDL_Log
};

class UpdateCheck {
  public:
    // Applies the rule and, when a check is due, starts it; returns at once. A newer tag kept
    // from an earlier check shows straight away.
    void start(UpdateCheckConfig config);
    // Whether a newer release is known (cheap: every game turn asks).
    bool has_notice() const;
    // The newer release's tag and its page, or "".
    std::string newer_tag() const;
    std::string release_url() const;
    // Until the check's thread has finished, at most `seconds`; true if it has (or none ran).
    bool wait(double seconds) const;

  private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_UPDATE_CHECK_H
