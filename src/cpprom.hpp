#include <algorithm>
#include <atomic>
#include <cassert>
#include <charconv>
#include <map>
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
using LabelValues = std::vector<std::string>;

namespace detail {
    struct NullMutex {
        void lock() const { } // BasicLockable
        void unlock() const { } // BasicLockable
        bool try_lock() const { return true; } // Lockable
    };

    bool isAlpha(char ch)
    {
        return (ch >= 'a' && ch <= 'z') && (ch >= 'A' && ch <= 'Z');
    }

    bool isDigit(char ch)
    {
        return ch >= '0' && ch <= '9';
    }

    bool isValidMetricName(std::string_view str)
    {
        // regex: [a-zA-Z_:][a-zA-Z0-9_:]*
        // I should probably use LUTs to check the characcters
        if (str.empty()) {
            return false;
        }
        // [a-zA-Z_:]
        if (!isAlpha(str[0]) && str[0] != '_' && str[0] != ':') {
            return false;
        }
        // [a-zA-Z0-9_:]*
        for (const auto ch : str.substr(1)) {
            if (!isAlpha(ch) && !isDigit(ch) && ch != '_' && ch != ':') {
                return false;
            }
        }
        return true;
    }

    bool isValidLabelName(std::string_view str)
    {
        // regex: [a-zA-Z_][a-zA-Z0-9_]*
        if (str.empty()) {
            return true;
        }
        if (str.size() >= 2 && str[0] == '_' && str[1] == '_') {
            // reserved for internal use
            return false;
        }
        // [a-zA-Z_]
        if (!isAlpha(str[0]) && str[0] != '_') {
            return false;
        }
        // [a-zA-Z0-9_]*
        for (const auto ch : str.substr(1)) {
            if (!isAlpha(ch) && !isDigit(ch) && ch != '_') {
                return false;
            }
        }
        return true;
    }

    void atomicAdd(std::atomic<double>& value, double delta)
    {
        auto current = value.load();
        while (!value.compare_exchange_weak(current, current + delta)) {
            // pass
        }
    }

    // https://github.com/boostorg/container_hash/blob/b3e424b6503709f4d86a91b78017ecce53747f02/include/boost/container_hash/hash.hpp#L340
    void hashCombine(size_t& seed, size_t v)
    {
        static_assert(sizeof(size_t) == 8);
        const size_t m = (size_t(0xc6a4a793) << 32) + 0x5bd1e995;
        const auto r = 47;
        v *= m;
        v ^= v >> r;
        v *= m;

        seed ^= v;
        seed *= m;
        seed += 0xe6546b64;
    }

    struct LabelValuesHash {
        size_t operator()(const LabelValues& labelValues) const
        {
            size_t seed = 0;
            for (const auto& v : labelValues) {
                hashCombine(seed, std::hash<std::string> {}(v));
            }
            return seed;
        }
    };

    struct MetricFamilyBase {
        virtual ~MetricFamilyBase() = default;
        virtual std::string serialize() const = 0;
    };

    std::string prefixComment(std::string_view name, std::string_view help, std::string_view type)
    {
        std::string str;
        if (help.size() > 0) {
            str.append("# HELP ");
            str.append(name);
            str.append(" ");
            str.append(help);
            str.append("\n");
        }
        str.append("# TYPE ");
        str.append(name);
        str.append(" ");
        str.append(type);
        str.append("\n");
        return str;
    }

    std::string serializeLabels(const std::vector<std::string>& names, const LabelValues& values,
        const std::string& le = "")
    {
        std::string str;
        assert(names.size() == values.size());
        if (values.size() > 0 || !le.empty()) {
            str.append("{");
            for (size_t i = 0; i < names.size(); ++i) {
                if (i > 0) {
                    str.append(",");
                }
                str.append(names[i]);
                str.append("=\"");
                str.append(values[i]);
                str.append("\"");
            }
            if (!le.empty()) {
                str.append("le=\"");
                str.append(le);
                str.append("\"");
            }
            str.append("}");
        }
        return str;
    }

    std::string toString(double num)
    {
        char buf[32]; // 15 significant digits plus decimal point
        const auto res = std::to_chars(buf, buf + sizeof(buf), num, std::chars_format::fixed);
        assert(res.ec == std::errc());
        return std::string(static_cast<const char*>(buf), const_cast<const char*>(res.ptr));
    }

    std::string toString(uint64_t num)
    {
        char buf[32]; // 20 digits is actually enough
        const auto res = std::to_chars(buf, buf + sizeof(buf), num);
        assert(res.ec == std::errc());
        return std::string(static_cast<const char*>(buf), const_cast<const char*>(res.ptr));
    }
}

class Counter {
public:
    explicit Counter(LabelValues labelValues)
        : labelValues_(std::move(labelValues))
    {
    }

    void inc(double delta = 1.0)
    {
        assert(delta > 0.0);
        detail::atomicAdd(value_, delta);
    }

    double value() const { return value_.load(); }

    const LabelValues& labelValues() const { return labelValues_; }

private:
    LabelValues labelValues_;
    std::atomic<double> value_ { 0.0 };
};

class Gauge {
public:
    struct TimeHandle { };
    struct TrackInProgressHandle { };

    explicit Gauge(LabelValues labelValues)
        : labelValues_(std::move(labelValues))
    {
    }

    void inc(double delta = 1.0) { detail::atomicAdd(value_, delta); }

    void dec(double delta = 1.0) { detail::atomicAdd(value_, -delta); }

    void set(double value) { value_.store(value); }

    void setToCurrentTime();
    TimeHandle time();
    TrackInProgressHandle trackInProgress();

    double value() const { return value_.load(); }

    const LabelValues& labelValues() const { return labelValues_; }

private:
    LabelValues labelValues_;
    std::atomic<double> value_ { 0.0 };
};

class Histogram {
public:
    static std::vector<double> defaultBuckets()
    {
        return std::vector<double> { 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0,
            10.0 };
    }

    static std::vector<double> linearBuckets(double start, double width, uint64_t count)
    {
        assert(count >= 1);
        std::vector<double> bounds { start };
        for (size_t i = 0; i < count - 1; ++i) {
            bounds.push_back(bounds.back() + width);
        }
        return bounds;
    }

    static std::vector<double> exponentialBuckets(double start, double factor, uint64_t count)
    {
        assert(count >= 1);
        std::vector<double> bounds { start };
        for (size_t i = 0; i < count - 1; ++i) {
            bounds.push_back(bounds.back() * factor);
        }
        return bounds;
    }

    struct Bucket {
        double upperBound;
        std::atomic<uint64_t> count { 0 };
    };

    struct TimeHandle { };

    template <typename Iterable>
    Histogram(LabelValues labelValues, const Iterable& bucketBounds)
        : labelValues_(std::move(labelValues))
        , buckets_(std::distance(std::begin(bucketBounds), std::end(bucketBounds)) + 1)
    {
        assert(buckets_.size() > 1);
        // This code could be much clearer/simpler if I could use buckets_.push_back, but sadly that
        // requires that Bucket is movable, which it is not, because of std::atomic. Therefore I
        // need to specify the size in the constructor already.
        size_t i = 0;
        for (const auto& boundary : bucketBounds) {
            buckets_[i].upperBound = static_cast<double>(boundary);
            if (i > 0) {
                assert(buckets_[i - 1].upperBound < static_cast<double>(boundary));
            }
            i++;
        }
        buckets_.back().upperBound = std::numeric_limits<double>::infinity();
    }

    void observe(double value)
    {
        for (auto& bucket : buckets_) {
            if (value <= bucket.upperBound) {
                ++bucket.count;
            }
        }
        detail::atomicAdd(sum_, value);
    }

    TimeHandle time();

    const std::vector<Bucket>& buckets() const { return buckets_; };

    double sum() const { return sum_; }
    uint64_t count() const { return buckets_.back().count.load(); }

private:
    LabelValues labelValues_;
    std::atomic<double> sum_ { 0.0 };
    std::vector<Bucket> buckets_;
};

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
    }

    template <typename = std::enable_if<std::is_same_v<Metric, Histogram>>>
    MetricFamily(std::string name, std::vector<std::string> labelNames,
        std::vector<double> bucketBounds, std::string help)
        : name_(std::move(name))
        , help_(std::move(help))
        , labelNames_(std::move(labelNames))
        , extraData_ { std::move(bucketBounds) }
    {
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
std::unique_ptr<Counter> MetricFamily<Counter>::newMetric(const LabelValues& labelValues)
{
    return std::make_unique<Counter>(labelValues);
}

template <>
std::string MetricFamily<Counter>::serialize() const
{
    std::string str = detail::prefixComment(name_, help_, "counter");
    for (const auto& [labels, metric] : metrics_) {
        str.append(name_);
        str.append(detail::serializeLabels(labelNames_, labels));
        str.append(" ");
        str.append(detail::toString(metric->value()));
        str.append("\n\n");
    }
    return str;
}

template <>
std::unique_ptr<Gauge> MetricFamily<Gauge>::newMetric(const LabelValues& labelValues)
{
    return std::make_unique<Gauge>(labelValues);
}

template <>
std::string MetricFamily<Gauge>::serialize() const
{
    std::string str = detail::prefixComment(name_, help_, "gauge");
    for (const auto& [labels, metric] : metrics_) {
        str.append(name_);
        str.append(detail::serializeLabels(labelNames_, labels));
        str.append(" ");
        str.append(detail::toString(metric->value()));
        str.append("\n\n");
    }
    return str;
}

template <>
std::unique_ptr<Histogram> MetricFamily<Histogram>::newMetric(const LabelValues& labelValues)
{
    return std::make_unique<Histogram>(labelValues, extraData_.bucketBounds);
}

template <>
std::string MetricFamily<Histogram>::serialize() const
{
    std::string str = detail::prefixComment(name_, help_, "histogram");
    for (const auto& [labels, metric] : metrics_) {
        for (const auto& bucket : metric->buckets()) {
            str.append(name_);
            str.append("_bucket");
            str.append(
                detail::serializeLabels(labelNames_, labels, detail::toString(bucket.upperBound)));
            str.append(" ");
            str.append(detail::toString(bucket.count.load()));
            str.append("\n");
        }

        str.append(name_);
        str.append("_sum");
        str.append(detail::serializeLabels(labelNames_, labels));
        str.append(" ");
        str.append(detail::toString(metric->sum()));
        str.append("\n");

        str.append(name_);
        str.append("_count");
        str.append(detail::serializeLabels(labelNames_, labels));
        str.append(" ");
        str.append(detail::toString(metric->buckets().back().count.load()));
        str.append("\n\n");
    }
    return str;
}

class Registry {
public:
    Registry(bool /*addDefaultCollectors*/ = true) { }

    MetricFamily<Counter>& counter(
        std::string name, std::vector<std::string> labelNames, std::string help)
    {
        return addFamily<Counter>(std::move(name), std::move(labelNames), std::move(help));
    }

    Counter& counter(std::string name, std::string help)
    {
        return counter(std::move(name), {}, std::move(help)).label({});
    }

    MetricFamily<Gauge>& gauge(
        std::string name, std::vector<std::string> labelNames, std::string help)
    {
        return addFamily<Gauge>(std::move(name), std::move(labelNames), std::move(help));
    }

    Gauge& gauge(std::string name, std::string help)
    {
        return gauge(std::move(name), {}, std::move(help)).label({});
    }

    MetricFamily<Histogram>& histogram(std::string name, std::vector<std::string> labelNames,
        std::vector<double> bucketBounds, std::string help)
    {
        assert(std::find(labelNames.begin(), labelNames.end(), "le") == labelNames.end());
        return addFamily<Histogram>(
            std::move(name), std::move(labelNames), std::move(bucketBounds), std::move(help));
    }

    Histogram& histogram(std::string name, std::vector<double> bucketBounds, std::string help)
    {
        return histogram(std::move(name), {}, std::move(bucketBounds), std::move(help)).label({});
    }

    // void remove(const std::string& name);

    std::string serialize() const
    {
        std::string str;
        for (const auto& [name, family] : families_) {
            str.append(family->serialize());
        }
        return str;
    }

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
