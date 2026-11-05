// The main thread's priority in a real-time run (#206, ADR 0120; thread_priority.h).
#include "thread_priority.h"

#include <SDL3/SDL.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif
#if defined(__linux__)
#include <cerrno>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <cctype>
#include <string_view>

namespace maplethief::frontend {

ThreadPriority current_thread_priority() {
    ThreadPriority p;
#if defined(_WIN32)
    const int value = GetThreadPriority(GetCurrentThread());
    if (value != THREAD_PRIORITY_ERROR_RETURN) {
        p.known = true;
        p.policy = "Windows";
        p.value = value;
    }
#else
    int policy = 0;
    sched_param param{};
    if (pthread_getschedparam(pthread_self(), &policy, &param) != 0) {
        return p;
    }
    p.known = true;
    p.value = param.sched_priority;
    switch (policy) {
    case SCHED_RR:
        p.policy = "SCHED_RR";
        break;
    case SCHED_FIFO:
        p.policy = "SCHED_FIFO";
        break;
    default:
        p.policy = "SCHED_OTHER";
#if defined(__linux__)
        // Linux's SCHED_OTHER has the one priority 0; the thread's nice value is what counts.
        errno = 0;
        const int nice_value =
            getpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)));
        if (errno == 0) {
            p.policy = "nice";
            p.value = nice_value;
        }
#endif
        break;
    }
#endif
    return p;
}

std::string describe(const ThreadPriority &priority) {
    if (!priority.known) {
        return "unknown";
    }
    const std::string_view policy = priority.policy;
    if (policy == "nice") {
        return "nice " + std::to_string(priority.value);
    }
    if (policy == "Windows") {
        const char *name = nullptr;
        switch (priority.value) {
        case -15:
            name = "THREAD_PRIORITY_IDLE";
            break;
        case -2:
            name = "THREAD_PRIORITY_LOWEST";
            break;
        case -1:
            name = "THREAD_PRIORITY_BELOW_NORMAL";
            break;
        case 0:
            name = "THREAD_PRIORITY_NORMAL";
            break;
        case 1:
            name = "THREAD_PRIORITY_ABOVE_NORMAL";
            break;
        case 2:
            name = "THREAD_PRIORITY_HIGHEST";
            break;
        case 15:
            name = "THREAD_PRIORITY_TIME_CRITICAL";
            break;
        default:
            break;
        }
        std::string text = "Windows priority " + std::to_string(priority.value);
        if (name != nullptr) {
            text += std::string(" (") + name + ")";
        }
        return text;
    }
    return std::string(policy) + " priority " + std::to_string(priority.value);
}

bool main_thread_priority_wanted(const char *env_value) {
    if (env_value == nullptr) {
        return true;
    }
    constexpr std::string_view kNormal = "normal";
    const std::string_view value = env_value;
    if (value.size() != kNormal.size()) {
        return true;
    }
    for (std::size_t i = 0; i < value.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(value[i])) != kNormal[i]) {
            return true;
        }
    }
    return false;
}

PriorityRaise raise_main_thread_priority(const std::string &why_not) {
    PriorityRaise r;
    r.before = current_thread_priority();
    r.why_not = why_not;
    r.asked = why_not.empty();
    if (r.asked) {
        r.raised = SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
        if (!r.raised) {
            r.error = SDL_GetError();
        }
    }
    r.after = current_thread_priority();
    return r;
}

std::string describe_raise(const PriorityRaise &r) {
    if (!r.asked) {
        return "main thread: " + describe(r.after) + ", the default (" + r.why_not + ")";
    }
    if (!r.raised) {
        return "main thread: SDL's high priority refused (" +
               (r.error.empty() ? std::string("no reason given") : r.error) + "); it keeps " +
               describe(r.after);
    }
    return "main thread: " + describe(r.after) + " (SDL's high priority; it was " +
           describe(r.before) + ")";
}

} // namespace maplethief::frontend
