#include "stats/stats_aggregator.h"
#include "stats/thread_level_stats_collector.h"
#include "util/test_harness.h"
#include "util/testing_stats_util.h"

namespace terrier {

/**
 * @brief Test the overall correctness of the stats framework
 */
class StatsFrameworkTests : public TerrierTest {};

/**
 * Basic test for testing metric registrction and stats collection
 */
// NOLINTNEXTLINE
TEST_F(StatsFrameworkTests, BasicTest) {
  auto stats_collector = stats::ThreadLevelStatsCollector();
  stats::ThreadLevelStatsCollector::GetCollectorForThread()->CollectTestNum(1);
  stats::ThreadLevelStatsCollector::GetCollectorForThread()->CollectTestNum(2);

  EXPECT_EQ(TestingStatsUtil::AggregateTestCounts(), 3);
}
}  // namespace terrier
