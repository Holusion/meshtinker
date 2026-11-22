#pragma once

#include <cstddef>
#include <string>

namespace meshtinker::common {

// Removes the registered paths if the process gets SIGINT, SIGTERM or SIGHUP,
// then dies of that signal as it would have. Destructors don't run on a
// signal, so without this an interrupted command would leave temporary
// outputs and workspaces behind. Commands call this once at startup.
void installInterruptCleanup();

// For commands that run others (docs/chaining.md#meta-commands): from now on,
// SIGINT, SIGTERM and SIGHUP are forwarded to the running child (see
// setForwardedChild) and recorded, and the process carries on: it waits for
// the child, which cleans up after itself, then sees interruption(), unwinds,
// and calls dieOfInterruption().
void deferInterruptions();

// The child to forward signals to, or 0 for none.
void setForwardedChild(long pid);

// The signal received while interruptions were deferred, or 0.
int interruption();

// Removes the registered paths and dies of the recorded signal, as an
// interrupted command would have.
[[noreturn]] void dieOfInterruption();

// Registers a file or directory for removal on interruption, for as long as
// the registration lives. Files are removed first, then directories, which
// must hold only registered paths by then, up to four levels deep. Removing
// it normally is still the owner's job.
// Up to 1024 paths can be registered at once; beyond that, paths are not
// cleaned up on interruption.
class CleanupRegistration {
public:
    explicit CleanupRegistration(std::string path);
    CleanupRegistration(CleanupRegistration&& other) noexcept;
    CleanupRegistration& operator=(CleanupRegistration&&) = delete;
    CleanupRegistration(const CleanupRegistration&) = delete;
    CleanupRegistration& operator=(const CleanupRegistration&) = delete;
    ~CleanupRegistration();

private:
    std::string path_;
    std::size_t slot_;
};

} // namespace meshtinker::common
