#include "cpprom/cpprom.hpp"

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

Counter::Counter(LabelValues labelValues, const Counter::Descriptor&)
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

Gauge::Gauge(LabelValues labelValues, const Gauge::Descriptor&)
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

Histogram::Histogram(LabelValues labelValues, const Histogram::Descriptor& descriptor)
    : labelValues_(std::move(labelValues))
    , buckets_(descriptor.bucketBounds.size() + 1) // +1 for +Inf bucket
{
    assert(buckets_.size() > 1);
    // This code could be much clearer/simpler if I could use buckets_.push_back, but sadly that
    // requires that Bucket is movable, which it is not, because of std::atomic. Therefore I
    // need to specify the size in the constructor already.
    size_t i = 0;
    for (const auto& boundary : descriptor.bucketBounds) {
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
    // https://prometheus.io/docs/concepts/data_model/#metric-names-and-labels

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
    std::string toString(double num)
    {
        if (num == std::numeric_limits<double>::infinity()) {
            return "+Inf";
        }

        char buf[32]; // 15 significant digits plus decimal point
        const auto res = std::to_chars(buf, buf + sizeof(buf), num, std::chars_format::fixed);
        assert(res.ec == std::errc());
        return std::string(static_cast<const char*>(buf), const_cast<const char*>(res.ptr));
    }
}

std::string serialize(const std::vector<Collector::Family>& families)
{
    // https://prometheus.io/docs/instrumenting/exposition_formats/
    // https://github.com/OpenObservability/OpenMetrics/blob/main/specification/OpenMetrics.md
    std::string str;
    str.reserve(4096);
    for (const auto& family : families) {
        if (family.help.size() > 0) {
            str.append("# HELP ");
            str.append(family.name);
            str.append(" ");
            str.append(family.help);
            str.append("\n");
        }
        str.append("# TYPE ");
        str.append(family.name);
        str.append(" ");
        str.append(family.type);
        str.append("\n");

        for (const auto& sample : family.samples) {
            str.append(sample.name);

            assert(sample.labelNames.size() == sample.labelValues.size());
            if (sample.labelValues.size() > 0) {
                str.append("{");
                for (size_t i = 0; i < sample.labelValues.size(); ++i) {
                    if (i > 0) {
                        str.append(",");
                    }
                    str.append(sample.labelNames[i]);
                    str.append("=\"");
                    // TODO: Worry about escaping this string
                    str.append(sample.labelValues[i]);
                    str.append("\"");
                }
                str.append("}");
            }

            str.append(" ");
            str.append(toString(sample.value));
            str.append("\n");
        }
        str.append("\n");
    }
    return str;
}

template <>
std::vector<Collector::Family> MetricFamily<Counter>::collect() const
{
    std::vector<Collector::Sample> samples;
    for (const auto& [labelValues, metric] : metrics_) {
        samples.push_back(Collector::Sample { name_, metric->value(), labelNames_, labelValues });
    }

    return std::vector<Collector::Family> {
        Collector::Family { name_, help_, "counter", std::move(samples) },
    };
}

template <>
std::vector<Collector::Family> MetricFamily<Gauge>::collect() const
{
    std::vector<Collector::Sample> samples;
    for (const auto& [labelValues, metric] : metrics_) {
        samples.push_back(Collector::Sample { name_, metric->value(), labelNames_, labelValues });
    }

    return std::vector<Collector::Family> {
        Collector::Family { name_, help_, "gauge", std::move(samples) },
    };
}

template <>
std::vector<Collector::Family> MetricFamily<Histogram>::collect() const
{
    for (const auto& labelName : labelNames_) {
        assert(labelName != "le");
    }

    std::vector<Collector::Sample> samples;
    std::lock_guard g(mutex_);
    for (const auto& [labelValues, metric] : metrics_) {
        auto bucketLabelNames = labelNames_;
        bucketLabelNames.push_back("le");

        auto bucketLabelValues = labelValues;
        bucketLabelValues.push_back("");

        const auto bucketName = name_ + "_bucket";
        for (const auto& bucket : metric->buckets()) {
            bucketLabelValues.back() = toString(bucket.upperBound);
            samples.push_back(Collector::Sample { bucketName,
                static_cast<double>(bucket.count.load()), bucketLabelNames, bucketLabelValues });
        }

        samples.push_back(
            Collector::Sample { name_ + "_sum", metric->sum(), labelNames_, labelValues });

        samples.push_back(Collector::Sample { name_ + "_count",
            static_cast<double>(metric->buckets().back().count.load()), labelNames_, labelValues });
    }

    return std::vector<Collector::Family> {
        Collector::Family { name_, help_, "histogram", std::move(samples) },
    };
}

std::shared_ptr<MetricFamily<Counter>> makeCounter(
    std::string name, std::vector<std::string> labelNames, std::string help)
{
    return std::make_shared<MetricFamily<Counter>>(
        std::move(name), std::move(labelNames), std::move(help));
}

std::shared_ptr<MetricFamily<Gauge>> makeGauge(
    std::string name, std::vector<std::string> labelNames, std::string help)
{
    return std::make_shared<MetricFamily<Gauge>>(
        std::move(name), std::move(labelNames), std::move(help));
}

std::shared_ptr<MetricFamily<Histogram>> makeHistogram(std::string name,
    std::vector<std::string> labelNames, std::vector<double> bucketBounds, std::string help)
{
    return std::make_shared<MetricFamily<Histogram>>(std::move(name), std::move(labelNames),
        std::move(help), Histogram::Descriptor { std::move(bucketBounds) });
}

Registry& Registry::getDefault()
{
    static Registry reg;
    return reg;
}

MetricFamily<Counter>& Registry::counter(
    std::string name, std::vector<std::string> labelNames, std::string help)
{
    auto f = makeCounter(std::move(name), std::move(labelNames), std::move(help));
    registerCollector(f);
    return *f;
}

Counter& Registry::counter(std::string name, std::string help)
{
    return counter(std::move(name), {}, std::move(help)).labels();
}

MetricFamily<Gauge>& Registry::gauge(
    std::string name, std::vector<std::string> labelNames, std::string help)
{
    auto f = makeGauge(std::move(name), std::move(labelNames), std::move(help));
    registerCollector(f);
    return *f;
}

Gauge& Registry::gauge(std::string name, std::string help)
{
    return gauge(std::move(name), {}, std::move(help)).labels();
}

MetricFamily<Histogram>& Registry::histogram(std::string name, std::vector<std::string> labelNames,
    std::vector<double> bucketBounds, std::string help)
{
    auto f = makeHistogram(
        std::move(name), std::move(labelNames), std::move(bucketBounds), std::move(help));
    registerCollector(f);
    return *f;
}

Histogram& Registry::histogram(std::string name, std::vector<double> bucketBounds, std::string help)
{
    return histogram(std::move(name), {}, std::move(bucketBounds), std::move(help)).labels();
}

Registry& Registry::registerCollector(std::shared_ptr<Collector> collector)
{
    std::lock_guard g(mutex_);
    assert(std::find(collectors_.begin(), collectors_.end(), collector) == collectors_.end());
    collectors_.push_back(collector);
    return *this;
}

std::string Registry::serialize() const
{
    std::string str;
    std::lock_guard g(mutex_);
    for (const auto& collector : collectors_) {
        str.append(cpprom::serialize(collector->collect()));
    }
    return str;
}
}
