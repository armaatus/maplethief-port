// One-time notices about the data folder (#262, #263, ADR 0127): the first-start copy ("Your
// saves and settings were copied from ..."), a refused copy, a fallback to the per-user folder.
// Whoever decides one queues it before the game starts; the game takes them once and shows them
// on the title screen (game.cpp: in the window, at the bottom left, for kDataNoticeSeconds from
// the first frame: the loading screen, then the title). Each is logged when queued. Thread-safe.
#ifndef MAPLETHIEF_PLATFORM_DATA_NOTICE_H
#define MAPLETHIEF_PLATFORM_DATA_NOTICE_H

#include <cstddef>
#include <string>
#include <vector>

namespace maplethief::frontend {

// How long the title screen shows them.
inline constexpr double kDataNoticeSeconds = 20.0;
// The width the window's text is wrapped to, in characters of the 8 x 8 debug font (the game's
// 480-point-wide screen holds 60).
inline constexpr std::size_t kDataNoticeColumns = 56;

// Queues `text` (one paragraph) for the title screen and logs it ("data: <text>").
void queue_data_notice(std::string text);
// Every notice queued since the last call, oldest first; the queue is empty afterwards. The
// title screen's display drains it; tests use it as the seam.
std::vector<std::string> take_data_notices();

// `text` broken into lines of at most `columns` characters at spaces; a word longer than that
// (a long path) is broken where it reaches the edge, after a '/' or '\' when it has one.
std::vector<std::string> wrap_notice(const std::string &text, std::size_t columns);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_DATA_NOTICE_H
