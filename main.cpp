#include <iostream>

#include "cpprom.hpp"

int main()
{
    cpprom::Registry reg;
    auto& reqTotal = reg.counter("http_requests_total", { "endpoint" }, "Total number of requests");
    auto& steps = reg.counter("steps_count", "Number of steps");
    auto& load = reg.gauge("load", "The load of something of course");
    auto& hist = reg.histogram("hist", cpprom::Histogram::defaultBuckets(),
        "A histogram to histogrammate things histogrammatically");

    load.set(12.0);
    load.set(69.0);

    hist.observe(2.0);
    hist.observe(8.0);
    hist.observe(69.0);
    hist.observe(42.0);

    reqTotal.label("/").inc();
    steps.inc();

    std::cout << reg.serialize() << std::endl;
}
