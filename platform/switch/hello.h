// The Nintendo Switch's hello path (#235, ADR 0138): what MapleThief.nro does until the runtime
// runs on Horizon (#236, #238). Shows "MapleThief <version>" on the console's screen with libnx's
// text console, sends the same line to nxlink's host when the NRO was started by nxlink
// (scripts/switch-run), and waits for + to return to the Homebrew Menu. No SDL: it checks the
// toolchain, the NRO and its start on hardware, nothing else.
#pragma once

namespace mt::switch_nx {

// The line shown and printed: "MapleThief <MT_VERSION>".
const char *hello_line();

// Runs the hello screen until + is pressed or the applet is closed. Returns the exit status (0).
int run_hello();

} // namespace mt::switch_nx
