#include "cpprom/processmetrics.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {
// This is only a small subset of relevant values
struct ProcStat {
    unsigned long utime; // (14) %lu - Amount of time scheduled in user mode
    unsigned long stime; // (15) %lu - Amount of time scheduled in kernel mode
    long num_threads; // (20) %ld
    unsigned long long starttime; // (22) %llu - Time in clock ticks the process started after boot
    unsigned long vsize; // (23) %lu - Virtual memory size in bytes
    long rss; // (24) %ld - Resident set size (number of pages in real memory)
};

std::optional<ProcStat> procStat()
{
    auto f = std::fopen("/proc/self/stat", "r");
    if (!f) {
        std::cerr << "Could not open /proc/self/stat" << std::endl;
        return std::nullopt;
    }
    ProcStat procStat;
    const auto r = std::fscanf(f,
        "%*d (%*[^)]) %*c %*d %*d %*d %*d %*d %*u %*u "
        "%*u %*u %*u %lu %lu %*d %*d %*d %*d %ld "
        "%*d %llu %lu %ld ",
        &procStat.utime, &procStat.stime, &procStat.num_threads, &procStat.starttime,
        &procStat.vsize, &procStat.rss);
    std::fclose(f);
    if (r != 6) {
        std::cerr << "Could not parse /proc/self/stat" << std::endl;
        return std::nullopt;
    }
    return procStat;
}

// If resource is a valid value, this should not fail
std::optional<unsigned long> getSoftRLimit(int resource)
{
    ::rlimit rlim;
    if (::getrlimit(resource, &rlim) == -1) {
        return std::nullopt;
    }
    return rlim.rlim_cur;
}

std::optional<size_t> countDir(const std::string& path)
{
    auto dir = ::opendir(path.c_str());
    if (!dir) {
        return std::nullopt;
    }

    ::dirent* ent;
    size_t count = 0;
    while ((ent = ::readdir(dir))) {
        // Ignore ., .. and potentially other garbage
        if (ent->d_type != DT_LNK) {
            continue;
        }
        count++;
    }
    ::closedir(dir);
    // We subtract 1, because opendir does openat/getdents64 etc in the background
    // so that during iteration we saw one extra fd from the directory being read.
    return count - 1;
}

std::optional<std::string> readFile(const std::string& path)
{
    auto fd = ::open(path.c_str(), O_RDONLY);
    if (fd == -1) {
        return std::nullopt;
    }
    char readBuffer[4096];
    std::string ret;
    int res = 0;
    while ((res = ::read(fd, readBuffer, sizeof(readBuffer))) > 0) {
        ret.append(std::string_view(readBuffer, res));
    }
    ::close(fd);
    if (res < 0) {
        return std::nullopt;
    }
    return ret;
}

// Boot time in seconds since the epoch
std::optional<int64_t> getBootTime()
{
    const auto file = readFile("/proc/stat");
    if (!file) {
        return std::nullopt;
    }
    const auto id = "btime ";
    const auto start = file->find(id);
    if (start == std::string::npos) {
        std::cerr << "Could not fine btime in /proc/stat" << std::endl;
        return std::nullopt;
    }

    int64_t btime;
    if (std::sscanf(file->data() + start, "btime %ld\n", &btime) != 1) {
        std::cerr << "Could not parse btime from /proc/stat" << std::endl;
        return std::nullopt;
    }
    return btime;
}

struct ProcessMetrics {
    std::optional<double> cpuSecondsTotal;
    std::optional<uint64_t> openFds;
    std::optional<uint64_t> maxFds;
    std::optional<uint64_t> virtualMemoryBytes;
    std::optional<uint64_t> virtualMemoryMaxBytes;
    std::optional<uint64_t> residentMemoryBytes;
    std::optional<double> startTimeSeconds;
    std::optional<uint64_t> threadCount;
};

ProcessMetrics getProcessMetrics()
{
    ProcessMetrics metrics;
    metrics.maxFds = getSoftRLimit(RLIMIT_NOFILE);
    metrics.virtualMemoryMaxBytes = getSoftRLimit(RLIMIT_AS);
    metrics.openFds = countDir("/proc/self/fd/");

    const auto stat = procStat();
    if (stat) {
        const auto clockTick = ::sysconf(_SC_CLK_TCK);
        assert(clockTick > 0);

        metrics.cpuSecondsTotal = static_cast<double>(stat->utime + stat->stime) / clockTick;
        metrics.virtualMemoryBytes = stat->vsize;
        metrics.residentMemoryBytes = stat->rss * ::getpagesize();
        metrics.threadCount = stat->num_threads;

        const auto bootTime = getBootTime();
        if (bootTime) {
            metrics.startTimeSeconds = *bootTime + static_cast<double>(stat->starttime) / clockTick;
        }
    }

    return metrics;
}

cpprom::Collector::Family makeFamily(
    std::string name, std::string help, std::string type, double value)
{
    return cpprom::Collector::Family {
        name,
        std::move(help),
        std::move(type),
        { cpprom::Collector::Sample { name, value, {}, {} } },
    };
}

struct ProcessMetricsCollector : public cpprom::Collector {
    std::vector<Family> collect() const override
    {
        std::vector<Family> families;

        const auto metrics = getProcessMetrics();

        if (metrics.cpuSecondsTotal) {
            families.push_back(makeFamily("process_cpu_seconds_total",
                "Total user and system CPU time spent in seconds.", "counter",
                *metrics.cpuSecondsTotal));
        }

        if (metrics.openFds) {
            families.push_back(makeFamily("process_open_fds", "Number of open file descriptors.",
                "gauge", static_cast<double>(*metrics.openFds)));
        }

        if (metrics.maxFds) {
            families.push_back(
                makeFamily("process_max_fds", "Maximum number of open file descriptors.", "gauge",
                    static_cast<double>(*metrics.maxFds)));
        }

        if (metrics.virtualMemoryBytes) {
            families.push_back(
                makeFamily("process_virtual_memory_bytes", "Virtual memory size in bytes.", "gauge",
                    static_cast<double>(*metrics.virtualMemoryBytes)));
        }

        if (metrics.virtualMemoryMaxBytes) {
            families.push_back(makeFamily("process_virtual_memory_max_bytes",
                "Maximum amount of virtual memory available in bytes.", "gauge",
                static_cast<double>(*metrics.virtualMemoryMaxBytes)));
        }

        if (metrics.residentMemoryBytes) {
            families.push_back(
                makeFamily("process_resident_memory_bytes", "Resident memory size in bytes.",
                    "gauge", static_cast<double>(*metrics.residentMemoryBytes)));
        }

        if (metrics.startTimeSeconds) {
            families.push_back(makeFamily("process_start_time_seconds",
                "Start time of the process since unix epoch in seconds.", "counter",
                *metrics.startTimeSeconds));
        }

        if (metrics.threadCount) {
            families.push_back(makeFamily("process_threads", "Number of OS threads in the process.",
                "gauge", static_cast<double>(*metrics.threadCount)));
        }

        return families;
    }
};
}

namespace cpprom {
std::shared_ptr<cpprom::Collector> makeProcessMetricsCollector()
{
    return std::make_shared<ProcessMetricsCollector>();
}
}
