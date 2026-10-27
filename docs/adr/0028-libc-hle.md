# 28. The libc subset: our own printf engine with iOS output, guest stdio, the device the guest sees, and abort as a trap

- **Status:** Accepted
- **Date:** 2026-10-27
- **Issue:** #44
- **Builds on:** [ADR 0019](0019-import-thunks-and-hle-abi.md) (registry, `mt_args`), [ADR 0025](0025-guest-memory-and-address-space.md) (guest heap, `mt_g2h`), [ADR 0027](0027-process-start-up.md) (`exit`, `__cxa_atexit`, the runtime's `mt_trap`)

## Context

The game imports 34 C-library functions and data objects from `libSystem.B.dylib` and 7 compiler-rt helpers from `libgcc_s.1.dylib` ([ANALYSIS.md → Imports](../ANALYSIS.md#imports-333), [re/imports.md](../re/imports.md)). The helpers came with #36. The rest are #44: `malloc/calloc/realloc/free`, `memcmp/memcpy/memset`, `strcat/strcmp/strcpy/strlen`, `atoi/atol`, `printf/puts/vfprintf/vsprintf/__snprintf_chk` with the data import `___stderrp`, `floor/pow/trunc`, `gettimeofday`, `sysctl/sysctlbyname`, `abort/__assert_rtn`, and the socket calls of `CMSocket` (`gethostbyname`, `inet_addr`, `inet_ntoa`, `setsockopt`, `close`, `if_nametoindex`).

Facts that shape the design:

- **Varargs are guest words.** A variadic argument follows ADR 0019's iOS rules: one word for `int`, `long`, `size_t` and pointers (ILP32), two 4-byte-aligned words for `long long` and `double`, split between r3 and the stack when they fall there. A `va_list` is a guest pointer to the next word. The host's `printf` cannot read any of that, and on 64-bit hosts its `long` is 8 bytes.
- **Hosts print differently.** iOS's libc is FreeBSD's `vfprintf`. It prints `%p` as `0x…`, `NULL` for `%s` as `(null)`, `nan` without a sign, pads `%s` and unknown conversions with zeros under `0`, gives `%%` a width, and formats `%a` with FreeBSD's `__hdtoa`, including its rounding of a dropped digit 8 (it looks at the parity of the *next* digit). glibc, MSVC's UCRT and FreeBSD disagree on most of these. Measured on macOS 26 (same code) in `tests/cpp/libc_test.cpp`.
- **The formatter has a second client.** `NSLog` and `+[NSString stringWithFormat:]` (#49) use the same conversions plus `%@`, which prints an object's description.
- **The device id needs `sysctl`.** `-[UIDevice(IdentifierAddition) macaddress]` (`0xC1025`) calls `if_nametoindex("en0")` and then `sysctl({CTL_NET, AF_ROUTE, 0, AF_LINK, NET_RT_IFLIST, index})` twice: once for the size, once for the data. It reads the MAC at `buf + 0x70 + 8 + sdl_nlen`, that is after the 112-byte armv7 `if_msghdr` and inside the `sockaddr_dl`. If this fails, the device id is nil and the `CStateTitle` constructor (`0x98865`) crashes in `strcpy` ([re/network.md](../re/network.md)). Chartboost asks `sysctlbyname("hw.machine")`.
- **No `fopen`.** The only `FILE *` the game uses is `___stderrp`, in Chartboost's `vfprintf` at `FUN_000ca668`. `___stderrp` is a 4-byte data import (ADR 0019).
- **`abort` is deep in guest call stacks.** Ending the host process from inside an HLE function would skip the platform layer's shutdown. `exit` (and `__cxa_atexit`) belong to process start-up ([ADR 0027](0027-process-start-up.md), #43): `mt_process_exit` runs the guest's atexit handlers and ends the run with a host `longjmp`. Any `mt_trap` ends the run the same way, as `MT_PROCESS_TRAPPED`.
- **The recompiler's C test harness** (`tests/recomp_harness.py`) compiles `registry.cpp` into a C program without the C++ runtime or guest memory. libc needs the guest heap.

## Options considered

**printf.**

1. **Rebuild a host format string per conversion and call the host's `snprintf` with host-typed values.** This is short, but `%p`, `%a`, inf/nan, the `0` flag on strings, and `%n` would follow the host, so the guest's text would depend on the OS.
2. **Our own engine with FreeBSD's semantics.** Parsing, integers, strings, `%p`, `%n`, inf/nan and `%a` are ours. Only finite `%e/%f/%g` go to the host's `snprintf` with a rebuilt spec and a `double`. The C standard pins those down, and every host we build on rounds them correctly.

**`abort`.** (a) Call the host's `abort`. (b) Stop the guest through `mt_trap` with a new trap kind, like every other way the guest stops.

**Harness.** (a) Give the harness test doubles for the heap. (b) Compile the registry there with a define that leaves out every module needing the rest of the runtime.

## Decision

- **Files.** The format engine is `runtime/src/hle/format.{h,cpp}`. The C library is `runtime/src/hle/libc.cpp`; sysctl and the network are `runtime/src/hle/libc_sys.cpp`. The public configuration is `runtime/include/maplethief/hle/libc.h`. Registry entries are added in `registry.cpp`.
- **The format engine** is option 2. Its interface is `int mt_format(cpu, fmt, mt_args *args, const mt_format_hooks *hooks, std::string &out)`. The arguments come from an `mt_args` cursor, so the same call serves variadic functions (the cursor continues after the fixed arguments) and `va_list`s (`mt_args_va_list`). It supports flags `- + space # 0 '`, `*` width and precision, the lengths `hh h l ll q j z t L` at the guest's sizes, and the conversions `d i o u x X c s p n % e E f F g G a A`, plus FreeBSD's `D O U C S`. `%lc`/`%ls` narrow in the C locale: a code point below 0x100 becomes that byte, anything else makes the call return −1, as on iOS. Positional arguments (`%1$d`) are not supported; nothing in the binary uses them.
- **`%@` is a hook.** `mt_format_hooks {conversions, fn, user}` routes the listed conversion characters to `fn`, which reads its own arguments from the cursor and appends its text. `mt_format_pad_string` applies width, precision and flags like `%s`. #49 passes `{"@", …}` (and may take over `%C`/`%S` for `unichar`). Without a hook, `%@` is an unknown conversion and prints `@`, as iOS's `printf` does.
- **Guest output.** `printf`, `puts`, `vfprintf` and `__assert_rtn` write through `mt_libc_config.write(user, fd, data, len)` for fd 1 or 2. The default is the host's stdout and stderr. `___stderrp` (and `___stdoutp`/`___stdinp`, which the game does not import) get guest `FILE` objects through init hooks: 88 bytes on the guest heap with iOS's `_flags` at +12 and `_file` at +14. `vfprintf` picks the stream by `_file`. Any other `FILE` returns −1 and is logged.
- **Semantics the guest can see follow iOS.** `memcpy` is overlap-safe. `strcmp`/`memcmp` return the difference of the first unequal bytes. `atoi`/`atol` saturate at 32 bits. `puts` returns `'\n'`. `gettimeofday` fills a 32-bit `timeval` from `mt_libc_config.clock` (default: the host's wall clock) and a zero time zone. `__snprintf_chk` aborts when the object is smaller than `maxlen`. A free or realloc of a pointer that is not a live block logs Darwin's `malloc: *** error for object …` line with the caller and aborts.
- **The device.** `sysctl`/`sysctlbyname` know `hw.machine`, `hw.model`, `hw.ncpu`, `hw.physmem` and `hw.memsize` for `mt_libc_config.hw_machine`. The default is `iPhone3,1` (iPhone 4); `MAPLETHIEF_HW_MACHINE` overrides it, and a small table gives the model code, CPUs and memory. `NET_RT_IFLIST` returns one `RTM_IFINFO` message for `en0` (index 2; `lo0` is 1) with the MAC `mt_libc_config.en0_mac`, default `02:00:00:00:00:00` (locally administered). Copy-out follows xnu: a NULL buffer asks for the size, and a short buffer gets what fits and fails with `*oldlenp` unchanged. Unknown nodes, and any attempt to set a value, return −1 and are logged.
- **Network (#23).** `gethostbyname` returns NULL. `inet_addr` and `inet_ntoa` are real: `inet_aton`'s parser, and a static 18-byte guest buffer. `setsockopt` and `close` return 0.
- **Aborting the guest** is option (b), with a new trap kind in `calls.h`. `abort`, `__assert_rtn` (after iOS's `Assertion failed: (…), function …, file …, line ….` on stderr), a malloc error and a failed `__*_chk` check call `mt_trap(cpu, MT_TRAP_ABORT (14), lr, detail)`, where detail is the offending pointer or 0. The runtime's `mt_trap` (ADR 0027, `runtime/src/boot/trap.c`) ends the run as trapped, and #53 turns it into a crash report. `exit` stays process start-up's.
- **Harness.** Option (b): `tests/recomp_harness.py` compiles the runtime's import sources with `MT_HLE_CORE_ONLY`. `registry.cpp` then lists only the core entries (libgcc, SjLj). Every module that needs the rest of the runtime goes inside the `#ifndef MT_HLE_CORE_ONLY` block.

## Consequences

- **Tests.** In CI, the doctest cases of `tests/cpp/libc_test.cpp` cover every function, called with its arguments laid out as the guest lays them out. The printf matrix runs each format through `printf` (registers, then stack, with 64-bit values split over r3 and the stack), `vsprintf` (`va_list`) and `__snprintf_chk` (varargs from `[sp+4]`). The cases where hosts agree are compared with the host's `snprintf`, with lengths mapped to the host's equivalent sizes. The iOS-only cases have fixed expected strings, which are also compared with the host's `printf` on Apple platforms. Locally, `tests/test_hle_libc_real.py` checks that every libSystem/libgcc import of the slice has a registry entry, apart from the ones other issues own (blocks, GCD, CommonCrypto, `__cxa_atexit`, SjLj; `exit` is #43's).
- **A new trap kind.** `MT_TRAP_ABORT` is how the guest ends itself on purpose. #53's crash report should tell it apart from a fault.
- **Reuse.** #49 builds `NSLog` and `stringWithFormat:` on `mt_format` with a `%@` hook. #53's trace can call `mt_format` too, to print a call's formatted text.
- **How we'd know this was wrong:**
  - **Text differs from the device.** A string the game builds with `vsprintf` differs from the device's, for example in a golden image of on-screen text. The fixed iOS cases come from macOS's libc, which shares the code but may have drifted since iOS 4.
  - **Two `printf` clients.** A second client needs a conversion the hook cannot express. Then the hook gets the raw spec text too.
  - **More `sysctl` nodes.** The game or a stub asks for a node we don't have. The log line names it.
