#include <iostream>

#include <cpprom/cpprom.hpp>

int main()
{
    auto& reg = cpprom::Registry::getDefault();
    auto& animalsSeen
        = reg.counter("animals_seen_total", { "animal", "color" }, "Number of animals seen");
    auto& steps = reg.counter("steps_total", "Number of steps");
    auto& cpuLoad = reg.gauge("cpu_load", "The CPU load");
    auto& hist = reg.histogram("hist", cpprom::Histogram::defaultBuckets(),
        "A histogram to histogrammate things histogrammatically");

    animalsSeen.labels("cat", "white").inc();

    auto& blueBirdSeen = animalsSeen.labels("bird", "blue"); // You can cache these
    blueBirdSeen.inc();

    steps.inc();
    cpuLoad.set(3.14159);
    for (size_t i = 0; i < 6; ++i) {
        hist.observe(i);
    }

    std::cout << reg.serialize() << std::endl;
}
