#include "orderbook/text.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace orderbook {
namespace {

NewOrder order(std::uint64_t id, Side side, OrderType type, std::int64_t quantity, std::int64_t price) {
  return NewOrder{OrderId{id}, side, type, Quantity{quantity}, Price{price}};
}

TEST(Text, FormatsCommandsAsTheDesignSays) {
  EXPECT_EQ(format(order(1, Side::Buy, OrderType::Limit, 10, 101)), "new 1 buy limit 10 @ 101");
  EXPECT_EQ(format(order(2, Side::Sell, OrderType::ImmediateOrCancel, 5, 99)), "new 2 sell ioc 5 @ 99");
  EXPECT_EQ(format(order(3, Side::Buy, OrderType::Market, 7, 0)), "new 3 buy market 7");
  EXPECT_EQ(format(CancelOrder{OrderId{4}}), "cancel 4");
}

TEST(Text, KeepsTheFieldsOfAnInvalidOrder) {
  // Zero and negative numbers, and a market order with a price, are commands the book must reject, so they need a
  // text form too.
  EXPECT_EQ(format(order(5, Side::Buy, OrderType::Limit, 0, -3)), "new 5 buy limit 0 @ -3");
  EXPECT_EQ(format(order(6, Side::Sell, OrderType::Market, -1, 50)), "new 6 sell market -1 @ 50");
}

TEST(Text, FormatsEventsAsTheDesignSays) {
  EXPECT_EQ(format(Accepted{OrderId{1}}), "accepted 1");
  EXPECT_EQ(format(Rejected{OrderId{2}, RejectReason::IdNotIncreasing}), "rejected 2 id-not-increasing");
  EXPECT_EQ(format(Rejected{OrderId{2}, RejectReason::InvalidQuantity}), "rejected 2 invalid-quantity");
  EXPECT_EQ(format(Rejected{OrderId{2}, RejectReason::InvalidPrice}), "rejected 2 invalid-price");
  EXPECT_EQ(format(Rejected{OrderId{2}, RejectReason::UnknownOrder}), "rejected 2 unknown-order");
  EXPECT_EQ(format(Trade{OrderId{3}, OrderId{1}, Quantity{10}, Price{101}}), "trade 3 1 10 @ 101");
  EXPECT_EQ(format(Cancelled{OrderId{3}, Quantity{5}, CancelReason::Unfilled}), "cancelled 3 5 unfilled");
  EXPECT_EQ(format(Cancelled{OrderId{1}, Quantity{10}, CancelReason::Requested}), "cancelled 1 10 requested");
}

TEST(Text, FormatsLevelsBestFirstAsGiven) {
  EXPECT_EQ(format(std::vector<Level>{}), "");
  EXPECT_EQ(format(std::vector<Level>{{Price{101}, Quantity{10}}}), "101x10");
  EXPECT_EQ(format(std::vector<Level>{{Price{101}, Quantity{10}}, {Price{100}, Quantity{5}}}), "101x10 100x5");
}

TEST(Text, ParsesEachCommandForm) {
  EXPECT_EQ(parse_command("new 1 buy limit 10 @ 101"), Command{order(1, Side::Buy, OrderType::Limit, 10, 101)});
  EXPECT_EQ(parse_command("new 2 sell ioc 5 @ 99"),
            Command{order(2, Side::Sell, OrderType::ImmediateOrCancel, 5, 99)});
  EXPECT_EQ(parse_command("new 3 buy market 7"), Command{order(3, Side::Buy, OrderType::Market, 7, 0)});
  EXPECT_EQ(parse_command("cancel 4"), Command{CancelOrder{OrderId{4}}});
}

TEST(Text, AcceptsAnySpacingBetweenWords) {
  EXPECT_EQ(parse_command("  new\t1  buy limit   10 @\t101  "),
            Command{order(1, Side::Buy, OrderType::Limit, 10, 101)});
  EXPECT_EQ(parse_command("\tcancel 4\t"), Command{CancelOrder{OrderId{4}}});
}

TEST(Text, AMarketOrderMayCarryAPriceForValidationToReject) {
  EXPECT_EQ(parse_command("new 6 sell market 1 @ 50"), Command{order(6, Side::Sell, OrderType::Market, 1, 50)});
  // "@ 0" is the same command as no price at all.
  EXPECT_EQ(parse_command("new 6 sell market 1 @ 0"), parse_command("new 6 sell market 1"));
}

TEST(Text, ParsesTheExtremesOfEachNumber) {
  EXPECT_EQ(parse_command("cancel 18446744073709551615"),
            Command{CancelOrder{OrderId{std::numeric_limits<std::uint64_t>::max()}}});
  EXPECT_EQ(parse_command("new 1 buy limit -9223372036854775808 @ 9223372036854775807"),
            Command{order(1, Side::Buy, OrderType::Limit, std::numeric_limits<std::int64_t>::min(),
                          std::numeric_limits<std::int64_t>::max())});
}

struct BadLine {
  const char* line;
  const char* error;
};

TEST(Text, RejectsMalformedLinesSayingWhy) {
  const std::array<BadLine, 20> cases{{
      {"", "empty command"},
      {"   ", "empty command"},
      {"buy 1 limit 10 @ 101", "unknown command 'buy'"},
      {"NEW 1 buy limit 10 @ 101", "unknown command 'NEW'"},
      {"new 1 buy limit 10", "limit orders need '@ <price>'"},
      {"new 1 buy ioc 10", "ioc orders need '@ <price>'"},
      {"new 1 buy limit 10 101", "expected: new"},
      {"new 1 buy limit 10 at 101", "expected '@' before the price, got 'at'"},
      {"new 1 buy limit 10 @ 101 extra", "expected: new"},
      {"new 1 bid limit 10 @ 101", "bad side 'bid'"},
      {"new 1 buy fok 10 @ 101", "bad order type 'fok'"},
      {"new x buy limit 10 @ 101", "bad order id 'x'"},
      {"new -1 buy limit 10 @ 101", "bad order id '-1'"},
      {"new 18446744073709551616 buy market 1", "bad order id '18446744073709551616'"},
      {"new 1 buy limit +10 @ 101", "bad quantity '+10'"},
      {"new 1 buy limit 0x10 @ 101", "bad quantity '0x10'"},
      {"new 1 buy limit 9223372036854775808 @ 101", "bad quantity '9223372036854775808'"},
      {"new 1 buy limit 10 @ 1.5", "bad price '1.5'"},
      {"cancel", "expected: cancel <id>"},
      {"cancel 1 2", "expected: cancel <id>"},
  }};
  for (const BadLine& c : cases) {
    std::string error;
    EXPECT_EQ(parse_command(c.line, &error), std::nullopt) << c.line;
    EXPECT_NE(error.find(c.error), std::string::npos) << "line '" << c.line << "' gave error '" << error << "'";
  }
  // The error is optional.
  EXPECT_EQ(parse_command("nonsense"), std::nullopt);
}

TEST(Text, ParseOfFormatIsTheSameCommandForAnyValues) {
  // std::mt19937_64's output is fixed by the standard, so this is the same sequence on every platform.
  std::mt19937_64 rng(20261016);
  constexpr std::array<std::int64_t, 7> edges{0, 1, -1, 1'000'000'000, 1'000'000'001,
                                             std::numeric_limits<std::int64_t>::min(),
                                             std::numeric_limits<std::int64_t>::max()};
  auto number = [&] {
    const std::uint64_t r = rng();
    return r % 4 == 0 ? edges[(r >> 8) % edges.size()] : static_cast<std::int64_t>(rng());
  };
  constexpr std::array<OrderType, 3> types{OrderType::Limit, OrderType::ImmediateOrCancel, OrderType::Market};
  for (int i = 0; i < 20'000; ++i) {
    const std::uint64_t r = rng();
    Command command;
    if (r % 5 == 0) {
      command = CancelOrder{OrderId{rng()}};
    } else {
      NewOrder o;
      o.id = OrderId{rng()};
      o.side = (r >> 3) % 2 == 0 ? Side::Buy : Side::Sell;
      o.type = types[(r >> 4) % types.size()];
      o.quantity = Quantity{number()};
      o.price = (r >> 6) % 3 == 0 && o.type == OrderType::Market ? Price{0} : Price{number()};
      command = o;
    }
    const std::string line = format(command);
    std::string error;
    ASSERT_EQ(parse_command(line, &error), command) << line << ": " << error;
  }
}

TEST(Text, ParsesLevels) {
  EXPECT_EQ(parse_levels(""), std::vector<Level>{});
  EXPECT_EQ(parse_levels("101x10"), (std::vector<Level>{{Price{101}, Quantity{10}}}));
  EXPECT_EQ(parse_levels(" 101x10  100x5 "), (std::vector<Level>{{Price{101}, Quantity{10}}, {Price{100}, Quantity{5}}}));
  for (const char* bad : {"101", "x5", "101x", "101x5x2", "101 x 5", "a1x5"}) {
    std::string error;
    EXPECT_EQ(parse_levels(bad, &error), std::nullopt) << bad;
    EXPECT_NE(error.find("expected <price>x<quantity>"), std::string::npos) << error;
  }
}

}  // namespace
}  // namespace orderbook
