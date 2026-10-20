// What the measuring tools (orderbook_latency, orderbook_loadgen) keep of a row of timed commands: nearest-rank
// percentiles of the samples themselves, for the printed table, and a histogram of them, written with --csv FILE for
// scripts/plot_latency.py.
//
// The histogram's buckets are log-linear, as in HdrHistogram: one a nanosecond below 64 ns (exact values), then 32 to
// each doubling ([64, 66), [66, 68), ..., [126, 128), [128, 132), ...), so from 32 ns up a bucket is at most 1/32
// (3.1%) of its lower bound wide, and the bounds are whole nanoseconds. 2^63 ns needs under 2,000 buckets.
#pragma once

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace orderbook::latency {

inline constexpr int kSubBits = 5;
inline constexpr std::uint64_t kSub = std::uint64_t{1} << kSubBits;  // buckets to a doubling
inline constexpr std::uint64_t kExact = 2 * kSub;                     // values below this have a bucket each

// The bucket a value falls in: the value itself below 64; above, how many doublings past 32 it is (e, from 1) and
// its top six bits (32 to 63), which pick one of the doubling's 32 buckets.
constexpr std::uint64_t bucket_of(std::uint64_t value) {
  if (value < kExact) return value;
  const int e = 63 - std::countl_zero(value) - kSubBits;  // countl_zero is an int in every library version
  return static_cast<std::uint64_t>(e) * kSub + (value >> e);
}

// A bucket's bounds: [lower_of(b), upper_of(b)).
constexpr std::uint64_t lower_of(std::uint64_t bucket) {
  if (bucket < kExact) return bucket;
  const std::uint64_t e = bucket / kSub - 1;
  return (bucket % kSub + kSub) << e;
}

constexpr std::uint64_t upper_of(std::uint64_t bucket) {
  if (bucket < kExact) return bucket + 1;
  return lower_of(bucket) + (std::uint64_t{1} << (bucket / kSub - 1));
}

struct Bucket {
  std::uint64_t lower = 0;  // ns, inclusive
  std::uint64_t upper = 0;  // ns, exclusive
  std::uint64_t count = 0;

  friend bool operator==(const Bucket&, const Bucket&) = default;
};

// The samples' non-empty buckets, lowest first. A negative sample (which a steady clock never gives) counts as 0.
inline std::vector<Bucket> histogram(const std::vector<std::int64_t>& samples) {
  std::vector<std::uint64_t> counts;
  for (const std::int64_t sample : samples) {
    const std::uint64_t bucket = bucket_of(sample < 0 ? 0 : static_cast<std::uint64_t>(sample));
    if (bucket >= counts.size()) counts.resize(bucket + 1, 0);
    ++counts[bucket];
  }
  std::vector<Bucket> buckets;
  for (std::uint64_t b = 0; b < counts.size(); ++b) {
    if (counts[b] != 0) buckets.push_back(Bucket{lower_of(b), upper_of(b), counts[b]});
  }
  return buckets;
}

// Nearest rank: the smallest sample with at least a fraction q of the samples at or below it (q in (0, 1]).
inline std::int64_t at_quantile(const std::vector<std::int64_t>& sorted, double q) {
  if (sorted.empty()) throw std::invalid_argument("no samples");
  const auto rank = static_cast<std::size_t>(q * static_cast<double>(sorted.size()) + 0.999999);
  return sorted[std::min(sorted.size(), std::max<std::size_t>(rank, 1)) - 1];
}

// A row of the table both tools print.
struct Summary {
  std::int64_t p50 = 0;
  std::int64_t p90 = 0;
  std::int64_t p99 = 0;
  std::int64_t p999 = 0;
  std::int64_t p9999 = 0;
  std::int64_t max = 0;
  double mean = 0;
};

// Sorts the samples.
inline Summary summarize(std::vector<std::int64_t>& samples) {
  std::sort(samples.begin(), samples.end());
  Summary summary{.p50 = at_quantile(samples, 0.50),
                  .p90 = at_quantile(samples, 0.90),
                  .p99 = at_quantile(samples, 0.99),
                  .p999 = at_quantile(samples, 0.999),
                  .p9999 = at_quantile(samples, 0.9999),
                  .max = samples.back()};
  double sum = 0;
  for (const std::int64_t sample : samples) sum += static_cast<double>(sample);
  summary.mean = sum / static_cast<double>(samples.size());
  return summary;
}

// The CSV file of --csv: a header line, then a line for each non-empty bucket of each row,
//   section,row,size,lower_ns,upper_ns,count
// where `size` is the row's book size or connection count. Names may not hold commas, quotes or line breaks.
class CsvWriter {
 public:
  CsvWriter() = default;
  explicit CsvWriter(const std::string& path) { open(path); }
  CsvWriter(const CsvWriter&) = delete;
  CsvWriter& operator=(const CsvWriter&) = delete;
  ~CsvWriter() {
    if (file_ != nullptr) std::fclose(file_);
  }

  // Starts a writer made without a path writing to `path`, from its header line.
  void open(const std::string& path) {
    if (file_ != nullptr) throw std::logic_error("the CSV file is already open");
    file_ = std::fopen(path.c_str(), "w");
    if (file_ == nullptr) throw std::runtime_error("can't write " + path);
    std::fputs("section,row,size,lower_ns,upper_ns,count\n", file_);
  }

  // A writer made without a path checks the names and writes nothing, so a run without --csv checks them too.
  void write(const std::string& section, const std::string& row, std::size_t size,
             const std::vector<std::int64_t>& samples) {
    for (const std::string* name : {&section, &row}) {
      if (name->find_first_of(",\"\r\n") != std::string::npos) throw std::invalid_argument("not a CSV name: " + *name);
    }
    if (file_ == nullptr) return;
    for (const Bucket& bucket : histogram(samples)) {
      std::fprintf(file_, "%s,%s,%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", section.c_str(), row.c_str(), size,
                   bucket.lower, bucket.upper, bucket.count);
    }
    if (std::fflush(file_) != 0) throw std::runtime_error("writing the CSV file failed");
  }

 private:
  std::FILE* file_ = nullptr;
};

}  // namespace orderbook::latency
