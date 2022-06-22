#include "cpprom.hpp"

#include <algorithm>
#include <charconv>

namespace {
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

bool isAlpha(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

bool isDigit(char ch)
{
    return ch >= '0' && ch <= '9';
}

template <typename Unit = std::chrono::seconds>
double now()
{
    return std::chrono::duration_cast<std::chrono::duration<double, typename Unit::period>>(
        std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
}
}

namespace cpprom {

namespace {
    void atomicAdd(std::atomic<double>& value, double delta)
    {
        auto current = value.load();
        while (!value.compare_exchange_weak(current, current + delta)) {
            // pass
        }
    }
}

Counter::Counter(LabelValues labelValues)
    : labelValues_(std::move(labelValues))
{
}

void Counter::inc(double delta)
{
    assert(delta > 0.0);
    atomicAdd(value_, delta);
}

double Counter::value() const
{
    return value_.load();
}

const LabelValues& Counter::labelValues() const
{
    return labelValues_;
}

Gauge::TimeHandle::TimeHandle(Gauge& gauge)
    : gauge(gauge)
    , start(now())
{
}

Gauge::TimeHandle::~TimeHandle()
{
    gauge.set(now() - start);
}

Gauge::TrackInProgressHandle::TrackInProgressHandle(Gauge& gauge)
    : gauge(gauge)
{
    gauge.inc();
}

Gauge::TrackInProgressHandle::~TrackInProgressHandle()
{
    gauge.dec();
}

Gauge::Gauge(LabelValues labelValues)
    : labelValues_(std::move(labelValues))
{
}

void Gauge::inc(double delta)
{
    atomicAdd(value_, delta);
}

void Gauge::dec(double delta)
{
    atomicAdd(value_, -delta);
}

void Gauge::set(double value)
{
    value_.store(value);
}

void Gauge::setToCurrentTime()
{
    set(now());
}

Gauge::TimeHandle Gauge::time()
{
    return TimeHandle(*this);
}

Gauge::TrackInProgressHandle Gauge::trackInProgress()
{
    return TrackInProgressHandle(*this);
}

double Gauge::value() const
{
    return value_.load();
}

const LabelValues& Gauge::labelValues() const
{
    return labelValues_;
}

std::vector<double> Histogram::defaultBuckets()
{
    return std::vector<double> { 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0 };
}

std::vector<double> Histogram::linearBuckets(double start, double width, uint64_t count)
{
    assert(count >= 1);
    std::vector<double> bounds { start };
    for (size_t i = 0; i < count - 1; ++i) {
        bounds.push_back(bounds.back() + width);
    }
    return bounds;
}

std::vector<double> Histogram::exponentialBuckets(double start, double factor, uint64_t count)
{
    assert(count >= 1);
    std::vector<double> bounds { start };
    for (size_t i = 0; i < count - 1; ++i) {
        bounds.push_back(bounds.back() * factor);
    }
    return bounds;
}

Histogram::TimeHandle::TimeHandle(Histogram& histogram)
    : histogram(histogram)
    , start(now())
{
}

Histogram::TimeHandle::~TimeHandle()
{
    histogram.observe(now() - start);
}

Histogram::Histogram(LabelValues labelValues, const std::vector<double>& bucketBounds)
    : labelValues_(std::move(labelValues))
    , buckets_(bucketBounds.size() + 1) // +1 for +Inf bucket
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

void Histogram::observe(double value)
{
    for (auto& bucket : buckets_) {
        if (value <= bucket.upperBound) {
            ++bucket.count;
        }
    }
    atomicAdd(sum_, value);
}

Histogram::TimeHandle Histogram::time()
{
    return Histogram::TimeHandle(*this);
}

const std::vector<Histogram::Bucket>& Histogram::buckets() const
{
    return buckets_;
}

double Histogram::sum() const
{
    return sum_;
}

uint64_t Histogram::count() const
{
    return buckets_.back().count.load();
}

namespace detail {
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

    size_t LabelValuesHash::operator()(const LabelValues& labelValues) const
    {
        size_t seed = 0;
        for (const auto& v : labelValues) {
            hashCombine(seed, std::hash<std::string> {}(v));
        }
        return seed;
    }
}

namespace {
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

template <>
std::unique_ptr<Counter> MetricFamily<Counter>::newMetric(const LabelValues& labelValues)
{
    return std::make_unique<Counter>(labelValues);
}

template <>
std::string MetricFamily<Counter>::serialize() const
{
    std::string str = prefixComment(name_, help_, "counter");
    for (const auto& [labels, metric] : metrics_) {
        str.append(name_);
        str.append(serializeLabels(labelNames_, labels));
        str.append(" ");
        str.append(toString(metric->value()));
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
    std::string str = prefixComment(name_, help_, "gauge");
    for (const auto& [labels, metric] : metrics_) {
        str.append(name_);
        str.append(serializeLabels(labelNames_, labels));
        str.append(" ");
        str.append(toString(metric->value()));
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
    std::string str = prefixComment(name_, help_, "histogram");
    for (const auto& [labels, metric] : metrics_) {
        for (const auto& bucket : metric->buckets()) {
            str.append(name_);
            str.append("_bucket");
            str.append(serializeLabels(labelNames_, labels, toString(bucket.upperBound)));
            str.append(" ");
            str.append(toString(bucket.count.load()));
            str.append("\n");
        }

        str.append(name_);
        str.append("_sum");
        str.append(serializeLabels(labelNames_, labels));
        str.append(" ");
        str.append(toString(metric->sum()));
        str.append("\n");

        str.append(name_);
        str.append("_count");
        str.append(serializeLabels(labelNames_, labels));
        str.append(" ");
        str.append(toString(metric->buckets().back().count.load()));
        str.append("\n\n");
    }
    return str;
}

MetricFamily<Counter>& Registry::counter(
    std::string name, std::vector<std::string> labelNames, std::string help)
{
    return addFamily<Counter>(std::move(name), std::move(labelNames), std::move(help));
}

Counter& Registry::counter(std::string name, std::string help)
{
    return counter(std::move(name), {}, std::move(help)).label({});
}

MetricFamily<Gauge>& Registry::gauge(
    std::string name, std::vector<std::string> labelNames, std::string help)
{
    return addFamily<Gauge>(std::move(name), std::move(labelNames), std::move(help));
}

Gauge& Registry::gauge(std::string name, std::string help)
{
    return gauge(std::move(name), {}, std::move(help)).label({});
}

MetricFamily<Histogram>& Registry::histogram(std::string name, std::vector<std::string> labelNames,
    std::vector<double> bucketBounds, std::string help)
{
    assert(std::find(labelNames.begin(), labelNames.end(), "le") == labelNames.end());
    return addFamily<Histogram>(
        std::move(name), std::move(labelNames), std::move(bucketBounds), std::move(help));
}

Histogram& Registry::histogram(std::string name, std::vector<double> bucketBounds, std::string help)
{
    return histogram(std::move(name), {}, std::move(bucketBounds), std::move(help)).label({});
}

// void remove(const std::string& name);

std::string Registry::serialize() const
{
    std::string str;
    for (const auto& [name, family] : families_) {
        str.append(family->serialize());
    }
    return str;
}

}
