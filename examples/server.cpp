#include <array>
#include <functional>
#include <iostream>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cpprom/cpprom.hpp>
#include <cpprom/processmetrics.hpp>

struct Metrics {
    cpprom::MetricFamily<cpprom::Counter>& reqsTotal;
    cpprom::MetricFamily<cpprom::Counter>& recvErrors;
    cpprom::MetricFamily<cpprom::Counter>& sendErrors;
    cpprom::MetricFamily<cpprom::Counter>& acceptErrors;
    cpprom::Gauge& reqsInFlight; // Always 1
    cpprom::MetricFamily<cpprom::Histogram>& reqSize;
    cpprom::MetricFamily<cpprom::Histogram>& reqDuration;

    static Metrics& get()
    {
        static auto& reg = cpprom::Registry::getDefault().registerCollector(
            cpprom::makeProcessMetricsCollector());
        static Metrics metrics {
            reg.counter("http_requests_total", { "method", "uri" }, "Number of requests received"),
            reg.counter("http_receive_errors_total", { "errno" }, "Number of receive errors"),
            reg.counter("http_send_errors_total", { "errno" }, "Number of send errors"),
            reg.counter("http_accept_errors_total", { "errno" }, "Number of accept errors"),
            reg.gauge("http_requests_in_flight", "Number of requests in flight"),
            reg.histogram("http_request_size_bytes", { "method", "uri" },
                cpprom::Histogram::exponentialBuckets(256.0, 2.0, 5), "HTTP request size"),
            reg.histogram("http_request_duration_seconds", { "method", "uri" },
                cpprom::Histogram::defaultBuckets(), "Time taken to process a HTTP request"),
        };
        return metrics;
    }
};

// This is of course the most minimal and primitive "HTTP server" you could build.
// Do not use for real, please.
void serve(uint16_t port, std::function<std::string(std::string_view)> handler)
{
    const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);

    const int reuseAddr = 1;
    if (::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr)) == -1) {
        std::cerr << "Error in setsockopt(SO_REUSEADDR): " << errno << std::endl;
        return;
    }

    ::sockaddr_in sa { AF_INET, ::htons(port), { ::htonl(INADDR_ANY) }, { 0 } };
    if (::bind(socket, reinterpret_cast<const ::sockaddr*>(&sa), sizeof(sa)) == -1) {
        std::cerr << "Error in bind: " << errno << "\n";
        return;
    }

    if (::listen(socket, SOMAXCONN) == -1) {
        std::cerr << "Error in listen: " << errno << "\n";
        return;
    }

    while (true) {
        const auto clientSocket = ::accept(socket, nullptr, nullptr);
        if (clientSocket == -1) {
            std::cerr << "Error in accept: " << errno << "\n";
            Metrics::get().acceptErrors.labels(std::to_string(errno)).inc();
            continue;
        }
        const auto trackInFlight = Metrics::get().reqsInFlight.trackInProgress();
        std::array<char, 4096> requestBuffer; // 4K ought to be enough for anybody
        ssize_t requestSize = ::recv(clientSocket, requestBuffer.data(), requestBuffer.size(), 0);
        if (requestSize < 0) {
            std::cerr << "Receive error: " << errno << "\n";
            Metrics::get().recvErrors.labels(std::to_string(errno)).inc();
        } else {
            std::string_view request(requestBuffer.data(), requestSize);
            std::cout << request.substr(0, request.find('\r')) << "\n"; // access log
            const auto delim = request.find(' ');
            const auto method = request.substr(0, delim);
            const auto uri = request.substr(delim + 1, request.find(' ', delim + 1) - delim - 1);
            Metrics::get().reqsTotal.labels(method, uri).inc();
            Metrics::get().reqSize.labels(method, uri).observe(requestSize);

            const auto durationHandle = Metrics::get().reqDuration.labels(method, uri).time();
            const auto response = handler(request);
            const ssize_t size = static_cast<ssize_t>(response.size());
            if (::send(clientSocket, response.data(), size, 0) < size) {
                std::cerr << "Send error: " << errno << "\n";
                Metrics::get().sendErrors.labels(std::to_string(errno)).inc();
            }
        }
        ::close(clientSocket);
    }
}

int main()
{
    serve(10069, [](std::string_view) {
        const auto body = cpprom::Registry::getDefault().serialize();
        const auto header = "HTTP/1.0 200 OK\r\n"
                            "Connection: close\r\n"
                            "Content-Type: text/plain; version=0.0.4\r\n"
                            "Content-Length: "
            + std::to_string(body.size()) + "\r\n\r\n";
        return header + body;
    });
    return 1;
}
