// The file dialogs' wrapper (#192, file_dialog.h; #182 reuses it): file:// URLs, the route to a
// dialog or the folder, and FileDialogs' hand-over: an answer from any thread waits for poll() on
// the caller's, one dialog at a time, and an answer after the wrapper is gone is dropped safely.
#include <doctest/doctest.h>

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "file_dialog.h"

namespace {

namespace fe = maplethief::frontend;
using Outcome = fe::FileDialogResult::Outcome;

} // namespace

TEST_CASE("file dialog: a folder as a file:// URL") {
    CHECK(fe::file_url("/Users/me/Library/Application Support/MapleThief") ==
          "file:///Users/me/Library/Application%20Support/MapleThief");
    CHECK(fe::file_url("C:\\Users\\Me\\AppData\\Roaming\\MapleThief") ==
          "file:///C:/Users/Me/AppData/Roaming/MapleThief");
    CHECK(fe::file_url("/home/me/.local/share/MapleThief") ==
          "file:///home/me/.local/share/MapleThief");
    // UTF-8 bytes, '#', '%', '?' and '&' are encoded; the unreserved set is kept.
    CHECK(fe::file_url("/tmp/caf\xC3\xA9 #1%?&a-b_c~d") ==
          "file:///tmp/caf%C3%A9%20%231%25%3F%26a-b_c~d");
    CHECK(fe::file_url("/a:b") == "file:///a%3Ab"); // only a drive's colon is kept
}

TEST_CASE("file dialog: where a dialog shows, and where the folder is used instead") {
    using fe::DialogPlace;
    using fe::DialogRoute;
    CHECK(fe::dialog_route(DialogPlace{}) == DialogRoute::kDialog);
    CHECK(fe::dialog_route(DialogPlace{.fullscreen = true}) == DialogRoute::kDialog);
    CHECK(fe::dialog_route(DialogPlace{.linux_os = true}) == DialogRoute::kDialog);
    CHECK(fe::dialog_route(DialogPlace{.linux_os = true, .fullscreen = true}) ==
          DialogRoute::kFolder);
    CHECK(fe::dialog_route(DialogPlace{.ios = true}) == DialogRoute::kFolder);
    CHECK(fe::dialog_route(DialogPlace{.gamescope = true}) == DialogRoute::kFolder);
    const DialogPlace here = fe::dialog_place(nullptr);
    CHECK_FALSE(here.fullscreen);
#if defined(SDL_PLATFORM_IOS)
    CHECK(here.ios);
#else
    CHECK_FALSE(here.ios);
#endif
}

TEST_CASE("file dialog: an answer from another thread is handed over at poll, once") {
    fe::FileDialogs dialogs;
    std::function<void(fe::FileDialogResult)> answer;
    fe::FileDialogRequest seen;
    dialogs.set_backend([&](SDL_Window *, const fe::FileDialogRequest &r,
                            std::function<void(fe::FileDialogResult)> a) {
        seen = r;
        answer = std::move(a);
        return true;
    });
    std::vector<fe::FileDialogResult> got;
    fe::FileDialogRequest req;
    req.kind = fe::FileDialogRequest::Kind::kSaveFile;
    req.title = "test";
    req.default_location = "/tmp/x.zip";
    req.filters = {{"Zip", "zip"}};
    std::string why;
    REQUIRE(
        dialogs.show(nullptr, req, [&](const fe::FileDialogResult &r) { got.push_back(r); }, &why));
    CHECK(seen.default_location == "/tmp/x.zip");
    CHECK(dialogs.busy());
    // One at a time.
    CHECK_FALSE(dialogs.show(nullptr, req, [](const fe::FileDialogResult &) {}, &why));
    CHECK(why == "a file dialog is already open");
    CHECK(dialogs.poll() == 0);
    std::thread t([&answer] { answer(fe::FileDialogResult{Outcome::kChosen, "/tmp/y.zip", ""}); });
    t.join();
    CHECK(got.empty()); // not on the answering thread
    CHECK(dialogs.poll() == 1);
    REQUIRE(got.size() == 1);
    CHECK(got[0].outcome == Outcome::kChosen);
    CHECK(got[0].path == "/tmp/y.zip");
    CHECK_FALSE(dialogs.busy());
    CHECK(dialogs.poll() == 0);

    // A second answer to the same request is ignored; the next dialog can open.
    REQUIRE(
        dialogs.show(nullptr, req, [&](const fe::FileDialogResult &r) { got.push_back(r); }, &why));
    auto first = answer;
    first(fe::FileDialogResult{Outcome::kCancelled, "", ""});
    first(fe::FileDialogResult{Outcome::kChosen, "/tmp/z.zip", ""});
    CHECK(dialogs.poll() == 1);
    REQUIRE(got.size() == 2);
    CHECK(got[1].outcome == Outcome::kCancelled);

    // A backend that cannot ask: false, never answered, not busy.
    dialogs.set_backend([](SDL_Window *, const fe::FileDialogRequest &,
                           std::function<void(fe::FileDialogResult)>) { return false; });
    CHECK_FALSE(
        dialogs.show(nullptr, req, [&](const fe::FileDialogResult &r) { got.push_back(r); }, &why));
    CHECK(why == "the file dialog cannot open");
    CHECK_FALSE(dialogs.busy());
    CHECK(got.size() == 2);
}

TEST_CASE("file dialog: an answer after the wrapper is gone is dropped") {
    std::function<void(fe::FileDialogResult)> answer;
    bool called = false;
    {
        fe::FileDialogs dialogs;
        dialogs.set_backend([&](SDL_Window *, const fe::FileDialogRequest &,
                                std::function<void(fe::FileDialogResult)> a) {
            answer = std::move(a);
            return true;
        });
        std::string why;
        REQUIRE(dialogs.show(
            nullptr, fe::FileDialogRequest{},
            [&called](const fe::FileDialogResult &) { called = true; }, &why));
    }
    answer(fe::FileDialogResult{Outcome::kChosen, "/tmp/late", ""});
    CHECK_FALSE(called);
}

TEST_CASE("file dialog: open_folder gives the URL to the opener, and its error back") {
    fe::FileDialogs dialogs;
    std::string url;
    dialogs.set_url_opener([&url](const std::string &u, std::string *why) {
        url = u;
        *why = "nope";
        return false;
    });
    std::string why;
    CHECK_FALSE(dialogs.open_folder("/tmp/a b", &why));
    CHECK(url == "file:///tmp/a%20b");
    CHECK(why == "nope");
    dialogs.set_url_opener([](const std::string &, std::string *) { return true; });
    CHECK(dialogs.open_folder("/tmp", &why));
}
