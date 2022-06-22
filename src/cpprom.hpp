#include <atomic>
#include <cassert>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

//#define CPPROM_SINGLE_THREADED
#ifdef CPPROM_SINGLE_THREADED
#define CPPROM_MUTEX NullMutex
#else
#include <mutex>
#define CPPROM_MUTEX std::mutex;
#endif

namespace cpprom {
namespace detail {
    // Just to disable moving and copying
    struct HandleBase {
        HandleBase() = default;
        HandleBase(const HandleBase&) = delete;
        HandleBase(HandleBase&& other) = delete;
        HandleBase& operator=(const HandleBase&) = delete;
        HandleBase& operator=(HandleBase&&) = delete;
    };
}

using LabelValues = std::vector<std::string>;

class Counter {
public:
    struct Descriptor { };

    explicit Counter(LabelValues labelValues);

    void inc(double delta = 1.0);

    double value() const;

    const LabelValues& labelValues() const;

private:
    LabelValues labelValues_;
    std::atomic<double> value_ { 0.0 };
};

class Gauge {
public:
    struct TimeHandle : public detail::HandleBase {
        Gauge& gauge;
        double start;

        TimeHandle(Gauge& gauge);
        ~TimeHandle();
    };

    struct TrackInProgressHandle : detail::HandleBase {
        Gauge& gauge;

        TrackInProgressHandle(Gauge& gauge);
        ~TrackInProgressHandle();
    };

    explicit Gauge(LabelValues labelValues);

    void inc(double delta = 1.0);

    void dec(double delta = 1.0);

    void set(double value);

    void setToCurrentTime();
    TimeHandle time();
    TrackInProgressHandle trackInProgress();

    double value() const;

    const LabelValues& labelValues() const;

private:
    LabelValues labelValues_;
    std::atomic<double> value_ { 0.0 };
};

class Histogram {
public:
    static std::vector<double> defaultBuckets();
    static std::vector<double> linearBuckets(double start, double width, uint64_t count);
    static std::vector<double> exponentialBuckets(double start, double factor, uint64_t count);

    struct Bucket {
        double upperBound;
        std::atomic<uint64_t> count { 0 };
    };

    struct TimeHandle : public detail::HandleBase {
        Histogram& histogram;
        double start;

        TimeHandle(Histogram& histogram);
        ~TimeHandle();
    };

    Histogram(LabelValues labelValues, const std::vector<double>& bucketBounds);

    void observe(double value);

    TimeHandle time();

    const std::vector<Bucket>& buckets() const;
    double sum() const;
    uint64_t count() const;

private:
    LabelValues labelValues_;
    std::atomic<double> sum_ { 0.0 };
    std::vector<Bucket> buckets_;
};

namespace detail {
    struct NullMutex {
        void lock() const { } // BasicLockable
        void unlock() const { } // BasicLockable
        bool try_lock() const { return true; } // Lockable
    };

    bool isValidMetricName(std::string_view str);
    bool isValidLabelName(std::string_view str);

    struct LabelValuesHash {
        size_t operator()(const LabelValues& labelValues) const;
    };

    struct MetricFamilyBase {
        virtual ~MetricFamilyBase() = default;
        virtual std::string serialize() const = 0;
    };
}

template <typename T>
struct MetricFamilyExtraData {
};

template <>
struct MetricFamilyExtraData<Histogram> {
    std::vector<double> bucketBounds;
};

template <typename Metric>
class MetricFamily : public detail::MetricFamilyBase {
public:
    MetricFamily(std::string name, std::vector<std::string> labelNames, std::string help)
        : name_(std::move(name))
        , help_(std::move(help))
        , labelNames_(std::move(labelNames))
    {
        assert(detail::isValidMetricName(name_));
        for (const auto& labelName : labelNames_) {
            assert(detail::isValidLabelName(labelName));
        }
    }

    template <typename = std::enable_if<std::is_same_v<Metric, Histogram>>>
    MetricFamily(std::string name, std::vector<std::string> labelNames,
        std::vector<double> bucketBounds, std::string help)
        : MetricFamily(std::move(name), std::move(labelNames), std::move(help))
    {
        extraData_ = MetricFamilyExtraData<Histogram> { std::move(bucketBounds) };
    }

    template <typename... Args>
    std::enable_if_t<(std::is_convertible_v<Args, std::string> && ...), Metric&> label(
        Args&&... args)
    {
        return label(std::vector<std::string> { std::forward<Args>(args)... });
    }

    Metric& label(const LabelValues& labelValues)
    {
        auto it = metrics_.find(labelValues);
        if (it == metrics_.end()) {
            it = metrics_.emplace(labelValues, newMetric(labelValues)).first;
        }
        return *it->second;
    }

    const auto& labelNames() const { return labelNames_; }
    const auto& metrics() const { return metrics_; }

    std::string serialize() const override;

private:
    std::unique_ptr<Metric> newMetric(const LabelValues& labelValues);

    std::string name_;
    std::string help_;
    std::vector<std::string> labelNames_;
    std::unordered_map<LabelValues, std::unique_ptr<Metric>, detail::LabelValuesHash> metrics_;
    MetricFamilyExtraData<Metric> extraData_;
};

template <>
std::unique_ptr<Counter> MetricFamily<Counter>::newMetric(const LabelValues& labelValues);

template <>
std::string MetricFamily<Counter>::serialize() const;

template <>
std::unique_ptr<Gauge> MetricFamily<Gauge>::newMetric(const LabelValues& labelValues);

template <>
std::string MetricFamily<Gauge>::serialize() const;

template <>
std::unique_ptr<Histogram> MetricFamily<Histogram>::newMetric(const LabelValues& labelValues);

template <>
std::string MetricFamily<Histogram>::serialize() const;

class Registry {
public:
    Registry(bool /*addDefaultCollectors*/ = true) { }

    MetricFamily<Counter>& counter(
        std::string name, std::vector<std::string> labelNames, std::string help);

    Counter& counter(std::string name, std::string help);

    MetricFamily<Gauge>& gauge(
        std::string name, std::vector<std::string> labelNames, std::string help);

    Gauge& gauge(std::string name, std::string help);

    MetricFamily<Histogram>& histogram(std::string name, std::vector<std::string> labelNames,
        std::vector<double> bucketBounds, std::string help);

    Histogram& histogram(std::string name, std::vector<double> bucketBounds, std::string help);

    std::string serialize() const;

private:
    template <typename Metric, typename... Args>
    MetricFamily<Metric>& addFamily(std::string name, Args&&... args)
    {
        const auto it = families_.find(name);
        assert(it == families_.end());
        std::string key = name;
        auto family
            = std::make_unique<MetricFamily<Metric>>(std::move(name), std::forward<Args>(args)...);
        auto& ref = *family;
        families_.emplace(std::move(key), std::move(family));
        return ref;
    }

    // I need to give out stable references, so I need a container with pointers that don't
    // invalidate after insertion / deletion. If I use a std::list, a linear search is not quite
    // as free as it would be with a vector, so std::map is actually not a bad choice. errors
    // with fucking map -> unordered of unique_ptr
    std::unordered_map<std::string, std::unique_ptr<detail::MetricFamilyBase>> families_;
};

}
