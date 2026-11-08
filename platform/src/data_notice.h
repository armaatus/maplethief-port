// One-time notices about the data folder (ADR 0127): shown once on the title screen and logged.
// #262 (the first-start copy) and #263 (the fallback notice) post through it.
//
// INTERIM (#263): #262 owns this module and its title-screen display; this is the shared
// interface (wave-3 design note) with a minimal queue, replaced by #262's when it lands.
#ifndef MAPLETHIEF_PLATFORM_DATA_NOTICE_H
#define MAPLETHIEF_PLATFORM_DATA_NOTICE_H

#include <string>
#include <vector>

namespace maplethief::frontend {

// Queues `text` to show once on the title screen; logs it.
void queue_data_notice(std::string text);
// The queued notices, oldest first; the queue is then empty (the display drains it; the tests'
// seam).
std::vector<std::string> take_data_notices();

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_DATA_NOTICE_H
