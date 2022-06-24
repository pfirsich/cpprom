# cpprom
A tiny C++17 [Prometheus](https://prometheus.io/) client library.

## Overview
```cpp
auto& reg = cpprom::Registry::getDefault();
auto& animalsSeen = reg.counter("animals_seen_total", { "animal", "color" }, "Number of animals seen");
auto& steps = reg.counter("steps_total", "Number of steps");
auto& cpuLoad = reg.gauge("cpu_load", "The CPU load");
auto& hist = reg.histogram("hist", cpprom::Histogram::defaultBuckets(), "A histogram to histogrammate things histogrammatically");

animalsSeen.labels("cat", "white").inc();

auto& blueBirdSeen = animalsSeen.labels("bird", "blue"); // You can cache these
blueBirdSeen.inc();

steps.inc();
cpuLoad.set(3.14159);
for (size_t i = 0; i < 6; ++i) {
    hist.observe(i);
}

std::cout << reg.serialize() << std::endl;
```

Output:
```
# HELP animals_seen_total Number of animals seen
# TYPE animals_seen_total counter
animals_seen_total{animal="bird",color="blue"} 1
animals_seen_total{animal="cat",color="white"} 1

# HELP steps_total Number of steps
# TYPE steps_total counter
steps_total 1

# HELP cpu_load The CPU load
# TYPE cpu_load gauge
cpu_load 3.14159

# HELP hist A histogram to histogrammate things histogrammatically
# TYPE hist histogram
hist_bucket{le="0.005"} 1
hist_bucket{le="0.01"} 1
hist_bucket{le="0.025"} 1
hist_bucket{le="0.05"} 1
hist_bucket{le="0.1"} 1
hist_bucket{le="0.25"} 1
hist_bucket{le="0.5"} 1
hist_bucket{le="1"} 2
hist_bucket{le="2.5"} 3
hist_bucket{le="5"} 6
hist_bucket{le="10"} 6
hist_bucket{le="+Inf"} 6
hist_sum 15
hist_count 6
```

For more information about usage, see [cpprom.hpp](include/cpprom/cpprom.hpp) and the [examples](examples/).
I consider this library fairly self-explanatory and small, so this is all the documentation there is for now.

Note that this library does **not** provide an HTTP server, because I don't think it's possible to add something that is one-size-fits-all.
Usually IO handling is very specific to the application and I myself would likely not even use it, if I added a HTTP server to this library.
Note that because the client is well known (Prometheus) it is much easier to build something that works, as you can see in the almost outrageous example [server.cpp](examples/server.cpp).

Prometheus has a fairly detailed page about [writing client libraries](https://prometheus.io/docs/instrumenting/writing_clientlibs) and I tried to stay fairly close to it.
The only `MUST` I did not respect is that `standard metrics MUST by default implicitly register into it with no special work required by the user`, because the process metrics are platform dependent, while the rest of the library is not.

## Thread-Safety
As required by the Prometheus documentation this library is thread-safe, but because I don't need it, there is a way to disable thread-safety and avoid locking and unlocking mutices needlessly (which might lead to expensive context switches).
If you use meson, you can set the `single_threaded` build option to `true`.
If you do not use meson, you can define `CPPROM_SINGLE_THREADED` (project-wide!).

Note that all methods on `Counter`, `Gauge` and `Histogram` are always thread-safe (even in single-threaded mode).
You only need thread-safety enabled, if you wish to call `MetricFamily::labels()` concurrently from multiple threads (likely) or if you wish to call any methods of `Registry` from multiple threads concurrently (not very likely).

## Building
If you use [meson](https://mesonbuild.com/) (it's very good), you can integrate this easily as a subproject by and using the `cpprom_dep` dependency object.

If you don't use meson, you can simply copy [cpprom.hpp](include/cpprom/cpprom.hpp) and [cpprom.cpp](src/cpprom.cpp) to your source tree.

Note that the default [process metrics](https://prometheus.io/docs/instrumenting/writing_clientlibs/#process-metrics) are not included in the `cpprom.*` files.
To use them, you also need to include [processmetrics.hpp](include/cpprom/processmetrics.hpp) and [processmetrics.cpp](src/processmetrics.cpp).
I do admit that this is somewhat unergonomic, but I am not quite sure how to structure it better. Let me know if you have suggestions.
Also please note that the process metrics implementation provided by this library only works on Linux.
