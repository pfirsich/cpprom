#include <functional>
#include <iostream>

#include "cpprom.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// This is of course the most minimal and primitive "HTTP server" you could build.
// Do not use for real, please.
bool serve(uint16_t port, std::function<std::string()> handler)
{
    const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);

    ::sockaddr_in sa { AF_INET, ::htons(port), { ::htonl(INADDR_ANY) }, { 0 } };

    const int reuseAddr = 1;
    if (::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr)) == -1) {
        std::cerr << "Error in setsockopt(SO_REUSEADDR)" << std::endl;
        return false;
    }

    if (::bind(socket, reinterpret_cast<const ::sockaddr*>(&sa), sizeof(sa)) == -1) {
        std::cerr << "Error binding to socket " << socket << "\n";
        return false;
    }

    if (::listen(socket, SOMAXCONN) == -1) {
        std::cerr << "Error listening on socket\n";
        return false;
    }

    while (true) {
        const auto clientSocket = ::accept(socket, nullptr, nullptr);
        const auto response = handler();
        const ssize_t size = static_cast<ssize_t>(response.size());
        if (::send(clientSocket, response.data(), size, 0) < size) {
            std::cerr << "Error sending response\n";
        }
        ::close(clientSocket);
    }
    return false;
}

int main()
{
    cpprom::Registry reg;
    auto& reqTotal = reg.counter("http_requests_total", { "endpoint" }, "Total number of requests");
    auto& steps = reg.counter("steps_count", "Number of steps");
    auto& load = reg.gauge("load", "The load of something of course");
    auto& timeTaken = reg.gauge("time_taken", "The time a long loop has taken");
    auto& inProgress = reg.gauge("in_progress", "Number of thingies in progress");
    auto& hist = reg.histogram("hist", cpprom::Histogram::defaultBuckets(),
        "A histogram to histogrammate things histogrammatically");

    load.set(12.0);
    load.set(69.0);

    hist.observe(2.0);
    hist.observe(8.0);
    hist.observe(69.0);
    hist.observe(42.0);

    {
        const auto h = timeTaken.time();
        ::usleep(1000 * 69);
    }

    const auto h1 = inProgress.trackInProgress();
    const auto h2 = inProgress.trackInProgress();
    {
        const auto h3 = inProgress.trackInProgress();
    }

    reqTotal.labels("/").inc();
    steps.inc();

    std::cout << reg.serialize() << std::endl;

    serve(10069, [&]() {
        const auto data = reg.serialize();
        std::cout << data << "\n";

        const std::string header = "HTTP/1.0 200 OK\r\nConnection: close\r\n"
                                   "Content-Type: text/plain; version=0.0.4\r\n"
                                   "Content-Length: "
            + std::to_string(data.size()) + "\r\n\r\n";
        return header + data;
    });
}
