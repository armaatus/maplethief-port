// The Nintendo Switch's side of every test NRO's main (switch_test_host.h, #236).
#include "switch_test_host.h"

#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <streambuf>
#include <string>

// MapleThiefTests.nro's forced guest fault (tests/cpp/switch_crash_test.cpp); absent from the
// other test NROs.
extern "C" int mt_test_switch_crash_test(void) __attribute__((weak));

namespace mt_test_switch {

namespace {

constexpr const char *k_dir = "sdmc:/switch/maplethief-tests";

// Everything doctest writes goes to the terminal (nxlink or the console) and the results file.
class Tee : public std::streambuf {
  public:
    explicit Tee(std::FILE *file) : file_(file) {}

  protected:
    int overflow(int c) override {
        if (c != EOF) {
            const char ch = static_cast<char>(c);
            std::fwrite(&ch, 1, 1, stdout);
            if (file_ != nullptr) {
                std::fwrite(&ch, 1, 1, file_);
            }
        }
        return c;
    }
    std::streamsize xsputn(const char *s, std::streamsize n) override {
        std::fwrite(s, 1, static_cast<std::size_t>(n), stdout);
        if (file_ != nullptr) {
            std::fwrite(s, 1, static_cast<std::size_t>(n), file_);
        }
        return n;
    }
    int sync() override {
        std::fflush(stdout);
        if (file_ != nullptr) {
            std::fflush(file_);
        }
        return 0;
    }

  private:
    std::FILE *file_;
};

struct State {
    bool sockets = false;
    int nxlink = -1;
    bool console = false;
    std::FILE *results = nullptr;
    Tee *tee = nullptr;
    std::ostream *out = nullptr;
    std::string results_path;
};
State g;

// "MapleThiefTests" from "sdmc:/switch/MapleThiefTests.nro".
std::string program_name(int argc, char **argv) {
    std::string name = argc > 0 && argv[0] != nullptr ? argv[0] : "";
    const std::size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) {
        name.erase(0, slash + 1u);
    }
    const std::size_t dot = name.rfind(".nro");
    if (dot != std::string::npos) {
        name.erase(dot);
    }
    return name;
}

void line(const char *text) {
    std::fprintf(stdout, "%s\n", text);
    if (g.results != nullptr) {
        std::fprintf(g.results, "%s\n", text);
    }
}

void environment_lines() {
    char text[160];
    std::string mode = appletGetAppletType() == AppletType_Application
                           ? "title takeover (Application)"
                           : "applet mode (" + std::to_string(appletGetAppletType()) + ")";
    std::string system = "Horizon";
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw{};
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            system += " " + std::to_string(fw.major) + "." + std::to_string(fw.minor) + "." +
                      std::to_string(fw.micro);
        }
        setsysExit();
    }
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    std::snprintf(text, sizeof text, "switch: %s, %s, memory %llu MiB of which %llu MiB used",
                  system.c_str(), mode.c_str(), static_cast<unsigned long long>(total >> 20),
                  static_cast<unsigned long long>(used >> 20));
    line(text);
    std::snprintf(text, sizeof text, "switch: results in %s", g.results_path.c_str());
    line(text);
}

} // namespace

std::ostream *begin(int argc, char **argv) {
    // nxlink: when scripts/switch-run sent the NRO, stdout and stderr go back to that computer.
    // Started from the Homebrew Menu there is none, and the console shows them instead.
    g.sockets = R_SUCCEEDED(socketInitializeDefault());
    g.nxlink = g.sockets ? nxlinkConnectToHost(true, true) : -1;
    if (g.nxlink < 0) {
        consoleInit(nullptr);
        g.console = true;
    }
    mkdir(k_dir, 0755);
    const std::string tmp = std::string(k_dir) + "/tmp";
    mkdir(tmp.c_str(), 0755);
    setenv("TMPDIR", tmp.c_str(), 1); // std::filesystem::temp_directory_path()

    const std::string name = program_name(argc, argv);
    g.results_path = std::string(k_dir) + "/" +
                     (name == "MapleThiefTests" || name.empty() ? "results" : name + "-results") +
                     ".txt";
    g.results = std::fopen(g.results_path.c_str(), "w");
    environment_lines();
    g.tee = new Tee(g.results);
    g.out = new std::ostream(g.tee);
    return g.out;
}

bool crash_test_requested(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (argv[i] != nullptr && std::strcmp(argv[i], "--crash-test") == 0) {
            return true;
        }
    }
    return false;
}

int run_crash_test() {
    if (mt_test_switch_crash_test == nullptr) {
        line("switch: --crash-test is MapleThiefTests.nro's");
        return 2;
    }
    if (g.results != nullptr) {
        std::fclose(g.results); // the crash ends the process: keep what is written
        g.results = nullptr;
    }
    return mt_test_switch_crash_test();
}

int end(int result) {
    if (g.out != nullptr) {
        g.out->flush();
    }
    char text[96];
    std::snprintf(text, sizeof text, "switch: %s (exit status %d)",
                  result == 0 ? "all tests passed" : "FAILED", result);
    line(text);
    if (g.results != nullptr) {
        std::fclose(g.results);
        g.results = nullptr;
    }
    if (g.console) {
        std::printf("\nPress + to return to the Homebrew Menu.\n");
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        PadState pad;
        padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) != 0) {
                break;
            }
            consoleUpdate(nullptr);
        }
        consoleExit(nullptr);
    }
    std::fflush(stdout);
    std::fflush(stderr);
    if (g.nxlink >= 0) {
        close(g.nxlink);
    }
    if (g.sockets) {
        socketExit();
    }
    return result;
}

} // namespace mt_test_switch
