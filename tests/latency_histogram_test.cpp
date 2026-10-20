// The measuring tools' histogram and percentiles (bench/latency_histogram.hpp).

#include "latency_histogram.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace orderbook::latency {
namespace {

// SplitMix64, for random values at every scale.
std::uint64_t splitmix64(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EB;
  return z ^ (z >> 31);
}

TEST(LatencyHistogram, ValuesBelow64HaveABucketEach) {
  for (std::uint64_t v = 0; v < 64; ++v) {
    EXPECT_EQ(bucket_of(v), v);
    EXPECT_EQ(lower_of(v), v);
    EXPECT_EQ(upper_of(v), v + 1);
  }
}

TEST(LatencyHistogram, BucketsWorkedOutByHand) {
  // 64: one doubling past 32 (e = 1), top six bits 32: the first bucket two wide.
  EXPECT_EQ(bucket_of(64), 64u);
  EXPECT_EQ(lower_of(64), 64u);
  EXPECT_EQ(upper_of(64), 66u);
  // 100 = 50 << 1; 1,000 = 62 << 4 plus 8; 1,000,000 = 61 << 14 plus 576.
  EXPECT_EQ(lower_of(bucket_of(100)), 100u);
  EXPECT_EQ(upper_of(bucket_of(100)), 102u);
  EXPECT_EQ(lower_of(bucket_of(1'000)), 992u);
  EXPECT_EQ(upper_of(bucket_of(1'000)), 1'008u);
  EXPECT_EQ(lower_of(bucket_of(1'000'000)), 999'424u);
  EXPECT_EQ(upper_of(bucket_of(1'000'000)), 1'015'808u);
  // The largest sample a steady clock's int64_t can hold: its bucket ends at 2^63, still a uint64_t.
  const auto largest = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(bucket_of(largest), 57u * 32 + 63);
  EXPECT_EQ(upper_of(bucket_of(largest)), std::uint64_t{1} << 63);
}

TEST(LatencyHistogram, BucketsAreContiguousAndAtMostAThirtySecondWide) {
  const std::uint64_t last = bucket_of(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
  for (std::uint64_t b = 0; b < last; ++b) {
    ASSERT_EQ(upper_of(b), lower_of(b + 1)) << "bucket " << b;
    ASSERT_LT(lower_of(b), upper_of(b)) << "bucket " << b;
    if (b >= 64) {
      ASSERT_LE((upper_of(b) - lower_of(b)) * 32, lower_of(b)) << "bucket " << b;
    }
  }
}

TEST(LatencyHistogram, EveryValueFallsInsideItsBucket) {
  for (std::uint64_t v = 0; v < 200'000; ++v) {
    const std::uint64_t b = bucket_of(v);
    ASSERT_LE(lower_of(b), v);
    ASSERT_LT(v, upper_of(b));
  }
  // And at every scale: random values with random magnitudes, up to 2^63 - 1.
  std::uint64_t state = 7;
  for (int i = 0; i < 200'000; ++i) {
    const std::uint64_t v = splitmix64(state) >> (1 + splitmix64(state) % 63);
    const std::uint64_t b = bucket_of(v);
    ASSERT_LE(lower_of(b), v) << v;
    ASSERT_LT(v, upper_of(b)) << v;
  }
}

TEST(LatencyHistogram, CountsEverySampleInItsBucketLowestFirst) {
  const std::vector<std::int64_t> samples{1'000, 0, 41, 65, 0, 42, 63, 64, 100, 101, -5};
  const std::vector<Bucket> expected{{0, 1, 3},  {41, 42, 1},   {42, 43, 1},  {63, 64, 1},
                                     {64, 66, 2}, {100, 102, 2}, {992, 1'008, 1}};
  EXPECT_EQ(histogram(samples), expected);  // the negative sample counts as 0
  EXPECT_TRUE(histogram({}).empty());
}

TEST(LatencyHistogram, NearestRankPercentiles) {
  std::vector<std::int64_t> hundred;
  for (std::int64_t v = 1; v <= 100; ++v) hundred.push_back(v);
  EXPECT_EQ(at_quantile(hundred, 0.001), 1);
  EXPECT_EQ(at_quantile(hundred, 0.50), 50);
  EXPECT_EQ(at_quantile(hundred, 0.90), 90);
  EXPECT_EQ(at_quantile(hundred, 0.99), 99);
  EXPECT_EQ(at_quantile(hundred, 0.999), 100);  // 99.9 samples: the rank rounds up
  EXPECT_EQ(at_quantile(hundred, 1.0), 100);

  // Where q * n has a fraction under a half, rounding to the nearest rank would be one too low: p90 of 16 samples is
  // the 15th (14.4 rounds up), p99 of 1,000 the 990th, p99.9 of 2,500 the 2,498th (2,497.5 up).
  std::vector<std::int64_t> sixteen;
  for (std::int64_t v = 1; v <= 16; ++v) sixteen.push_back(v);
  EXPECT_EQ(at_quantile(sixteen, 0.90), 15);
  EXPECT_EQ(at_quantile(sixteen, 0.50), 8);
  std::vector<std::int64_t> thousands;
  for (std::int64_t v = 1; v <= 2'500; ++v) thousands.push_back(v);
  EXPECT_EQ(at_quantile(thousands, 0.999), 2'498);
  EXPECT_EQ(at_quantile(thousands, 0.9), 2'250);

  std::vector<std::int64_t> one{7};
  EXPECT_EQ(at_quantile(one, 0.5), 7);
  EXPECT_THROW(at_quantile({}, 0.5), std::invalid_argument);
}

TEST(LatencyHistogram, SummarizeSortsAndTakesEachPercentile) {
  std::vector<std::int64_t> samples;
  for (std::int64_t v = 10'000; v >= 1; --v) samples.push_back(v);  // descending
  const Summary summary = summarize(samples);
  EXPECT_TRUE(std::is_sorted(samples.begin(), samples.end()));
  EXPECT_EQ(summary.p50, 5'000);
  EXPECT_EQ(summary.p90, 9'000);
  EXPECT_EQ(summary.p99, 9'900);
  EXPECT_EQ(summary.p999, 9'990);
  EXPECT_EQ(summary.p9999, 9'999);
  EXPECT_EQ(summary.max, 10'000);
  EXPECT_DOUBLE_EQ(summary.mean, 5'000.5);
}

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

TEST(LatencyHistogram, TheCsvFileHasALineForEachNonEmptyBucketOfEachRow) {
  const std::string path = testing::TempDir() + "latency_histogram_test.csv";
  {
    CsvWriter csv(path);
    csv.write("engine thread", "paced 1M/s", 1'000, {0, 0, 65, 1'000});
    csv.write("gateway", "one at a time", 1, {64});
  }
  EXPECT_EQ(read_file(path),
            "section,row,size,lower_ns,upper_ns,count\n"
            "engine thread,paced 1M/s,1000,0,1,2\n"
            "engine thread,paced 1M/s,1000,64,66,1\n"
            "engine thread,paced 1M/s,1000,992,1008,1\n"
            "gateway,one at a time,1,64,66,1\n");
}

TEST(LatencyHistogram, ACsvNameCantHoldACommaOrAQuote) {
  const std::string path = testing::TempDir() + "latency_histogram_names_test.csv";
  CsvWriter csv(path);
  EXPECT_THROW(csv.write("gateway", "paced 1,000/s", 1, {1}), std::invalid_argument);
  EXPECT_THROW(csv.write("a \"quoted\" one", "row", 1, {1}), std::invalid_argument);
  EXPECT_THROW(csv.write("gateway", "two\nlines", 1, {1}), std::invalid_argument);
}

TEST(LatencyHistogram, AWriterWithoutAFileWritesNothingButStillChecksNames) {
  CsvWriter none;
  EXPECT_NO_THROW(none.write("gateway", "paced 1000/s", 1, {1}));
  EXPECT_THROW(none.write("gateway", "paced 1,000/s", 1, {1}), std::invalid_argument);
  EXPECT_THROW(CsvWriter("/nonexistent-directory/x.csv"), std::runtime_error);
}

TEST(LatencyHistogram, AWriterOpenedLaterStartsWithTheHeader) {
  const std::string path = testing::TempDir() + "latency_histogram_open_test.csv";
  {
    CsvWriter csv;
    csv.write("gateway", "before", 1, {5});  // nowhere to go yet
    csv.open(path);
    csv.write("gateway", "after", 1, {5});
    EXPECT_THROW(csv.open(path), std::logic_error);
  }
  EXPECT_EQ(read_file(path), "section,row,size,lower_ns,upper_ns,count\ngateway,after,1,5,6,1\n");
}

}  // namespace
}  // namespace orderbook::latency
