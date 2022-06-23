#pragma once

#include "cpprom.hpp"

namespace cpprom {
/*

    https://prometheus.io/docs/instrumenting/writing_clientlibs/#process-metrics

    Exports the following metrics:

    process_cpu_seconds_total = (utime + stime) / userHZ
        with utime and stime from /proc/[fd]/stat and userHZ = sysconf(_SC_CLK_TCK)

    process_open_fds = num items in /proc/self/fd/
        it seems there is no better way from C(++)

    process_max_fds = getrlimit(RLIMIT_NOFILE) soft limit

    process_virtual_memory_bytes = vsize from /proc/[fd]/stat

    process_virtual_memory_max_bytes = getrlimit(RLIMIT_AS) soft limit

    process_resident_memory_bytes = rss * ::getpagesize()
        with rss from /proc/[fd]/stat

    process_start_time_seconds = btime + (starttime / userHZ)
        with starttime from /proc/[fd]/stat, userHZ = sysconf(_SC_CLK_TCK) and btime from /proc/stat

    process_threads = num_threads from /proc/[fd]/stat

    process_heap_bytes is omitted purposely, because I am not sure how to do it or if it makes sense

 */

std::shared_ptr<cpprom::Collector> makeProcessMetricsCollector();
}
