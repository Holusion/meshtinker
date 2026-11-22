#include "meshtinker/common/cleanup.hpp"

#include <array>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#define MESHTINKER_POSIX_SIGNALS 1
#endif

namespace meshtinker::common {
namespace {

constexpr std::size_t slot_count = 1024;
constexpr std::size_t no_slot = slot_count;

// The handler only reads these pointers and calls unlink and rmdir, which are
// async-signal-safe. The strings they point to belong to registrations.
std::array<std::atomic<const char*>, slot_count> slots{};

#if defined(MESHTINKER_POSIX_SIGNALS)
// How deep registered directories nest: a chain directory created by
// `import`, a stage or workspace directory in it.
constexpr int directory_depth = 4;

// Interruptions deferred (deferInterruptions), the child they go to, and the
// signal received. Lock-free atomics, which a handler may use.
std::atomic<bool> deferring{false};
std::atomic<long> forwarded_child{0};
std::atomic<int> received{0};

void removePaths() {
    // Files first, then directories, which must be empty by then. Slots
    // aren't in nesting order, so directories take one pass per level.
    for (std::atomic<const char*>& slot : slots) {
        const char* path = slot.load();
        if (path != nullptr) {
            ::unlink(path);
        }
    }
    for (int pass = 0; pass < directory_depth; ++pass) {
        for (std::atomic<const char*>& slot : slots) {
            const char* path = slot.load();
            if (path != nullptr) {
                ::rmdir(path);
            }
        }
    }
}

extern "C" void removeRegisteredPaths(int signal_number) {
    if (deferring.load()) {
        received.store(signal_number);
        const long child = forwarded_child.load();
        if (child > 0) {
            ::kill(static_cast<pid_t>(child), signal_number);
        }
        return;
    }
    removePaths();
    std::signal(signal_number, SIG_DFL);
    std::raise(signal_number);
}
#endif

} // namespace

void installInterruptCleanup() {
#if defined(MESHTINKER_POSIX_SIGNALS)
    for (const int signal_number : {SIGINT, SIGTERM, SIGHUP}) {
        std::signal(signal_number, removeRegisteredPaths);
    }
#endif
}

void deferInterruptions() {
#if defined(MESHTINKER_POSIX_SIGNALS)
    deferring.store(true);
#endif
}

void setForwardedChild(long pid) {
#if defined(MESHTINKER_POSIX_SIGNALS)
    forwarded_child.store(pid);
    // A signal that came just before the child started reaches it too.
    if (pid > 0 && received.load() != 0) {
        ::kill(static_cast<pid_t>(pid), received.load());
    }
#else
    (void)pid;
#endif
}

int interruption() {
#if defined(MESHTINKER_POSIX_SIGNALS)
    return received.load();
#else
    return 0;
#endif
}

void dieOfInterruption() {
#if defined(MESHTINKER_POSIX_SIGNALS)
    const int signal_number = received.load();
    deferring.store(false);
    removePaths();
    std::signal(signal_number, SIG_DFL);
    std::raise(signal_number);
#endif
    std::_Exit(128);
}

CleanupRegistration::CleanupRegistration(std::string path) : path_(std::move(path)), slot_(no_slot) {
    for (std::size_t i = 0; i < slot_count; ++i) {
        const char* expected = nullptr;
        if (slots[i].compare_exchange_strong(expected, path_.c_str())) {
            slot_ = i;
            return;
        }
    }
}

CleanupRegistration::CleanupRegistration(CleanupRegistration&& other) noexcept
    : path_(std::move(other.path_)), slot_(std::exchange(other.slot_, no_slot)) {
    // Moving the string may move its characters (short strings live inline).
    if (slot_ != no_slot) {
        slots[slot_].store(path_.c_str());
    }
}

CleanupRegistration::~CleanupRegistration() {
    if (slot_ != no_slot) {
        slots[slot_].store(nullptr);
    }
}

} // namespace meshtinker::common
