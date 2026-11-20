#include "meshtinker/image/image.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#if defined(__linux__)
#include <sched.h>
#endif

namespace meshtinker::image {

namespace {

#if defined(__linux__)
// The CPUs allowed by the cgroup v2 `cpu.max` quota of this process's cgroup,
// rounded up, or 0 when there is no quota.
unsigned cgroupQuota() {
    std::ifstream cgroup("/proc/self/cgroup");
    std::string line;
    std::string path;
    while (std::getline(cgroup, line)) {
        // cgroup v2's single line: `0::/path`.
        if (line.rfind("0::", 0) == 0) {
            path = line.substr(3);
        }
    }
    for (const std::string& candidate : {"/sys/fs/cgroup" + path + "/cpu.max", std::string("/sys/fs/cgroup/cpu.max")}) {
        std::ifstream file(candidate);
        std::string quota;
        double period = 0;
        if (!(file >> quota >> period)) {
            continue;
        }
        if (quota == "max" || period <= 0) {
            return 0;
        }
        const double cpus = std::ceil(std::stod(quota) / period);
        return cpus >= 1 ? static_cast<unsigned>(cpus) : 1;
    }
    return 0;
}
#endif

} // namespace

unsigned threadCount() {
    unsigned count = 0;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        count = static_cast<unsigned>(CPU_COUNT(&set));
    }
    if (const unsigned quota = cgroupQuota(); quota != 0 && (count == 0 || quota < count)) {
        count = quota;
    }
#endif
    if (count == 0) {
        count = std::thread::hardware_concurrency();
    }
    return std::max(1u, count);
}

} // namespace meshtinker::image
