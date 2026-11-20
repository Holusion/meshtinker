#include "meshtinker/common/cleanup.hpp"

#include <array>
#include <atomic>
#include <csignal>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
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

extern "C" void removeRegisteredPaths(int signal_number) {
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
