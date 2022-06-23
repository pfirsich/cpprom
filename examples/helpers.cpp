#include <chrono>
#include <iostream>
#include <thread>

#include <cpprom/cpprom.hpp>
#include <cpprom/processmetrics.hpp>

int main()
{
    auto& reg = cpprom::Registry::getDefault();
    auto& timeTaken = reg.gauge("last_time_taken_seconds", "The ");
    auto& inProgress = reg.gauge("in_progress_things", "Number of things in progress");
    auto& taskDuration = reg.histogram("task_duration_seconds", cpprom::Histogram::defaultBuckets(),
        "A histogram to histogrammate things histogrammatically");

    {
        // When it is destructed, the handle returned by time() will set the value of the gauge
        // metric to the time it has been alive.
        const auto h = timeTaken.time();
        std::this_thread::sleep_for(std::chrono::milliseconds(69));
    }

    // The handles returnd by trackInProgress increment on construction and decrement on
    // destruction.
    const auto h1 = inProgress.trackInProgress();
    const auto h2 = inProgress.trackInProgress();
    {
        const auto h3 = inProgress.trackInProgress();
    }

    for (const auto durationMs : std::vector<int64_t> { 42, 69, 404 }) {
        const auto h = taskDuration.time();
        std::this_thread::sleep_for(std::chrono::milliseconds(durationMs));
    }

    std::cout << reg.serialize() << std::endl;
}
