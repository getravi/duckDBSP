#include "catch.hpp"
#include "include/dbsp_zset.hpp"
#include "include/dbsp_stream.hpp"

using namespace dbsp;

TEST_CASE("ZSet basic operations", "[zset]") {
    ZSet<int> zs;

    SECTION("empty zset") {
        REQUIRE(zs.empty());
        REQUIRE(zs.support_size() == 0);
    }

    SECTION("insert elements") {
        zs.insert(1, 1);
        zs.insert(2, 2);
        zs.insert(3, -1);

        REQUIRE(zs[1] == 1);
        REQUIRE(zs[2] == 2);
        REQUIRE(zs[3] == -1);
        REQUIRE(zs[4] == 0);
        REQUIRE(zs.support_size() == 3);
    }

    SECTION("insert same element") {
        zs.insert(1, 2);
        zs.insert(1, 3);
        REQUIRE(zs[1] == 5);
    }

    SECTION("cancel out to zero") {
        zs.insert(3, 1);
        zs.insert(3, -1);
        REQUIRE(zs[3] == 0);
        REQUIRE_FALSE(zs.contains(3));
        REQUIRE(zs.support_size() == 0);
    }
}

TEST_CASE("ZSet arithmetic", "[zset]") {
    ZSet<int> zs1, zs2;
    zs1.insert(1, 2);
    zs1.insert(2, 3);
    zs2.insert(2, -1);
    zs2.insert(3, 4);

    SECTION("addition") {
        auto sum = zs1 + zs2;
        REQUIRE(sum[1] == 2);
        REQUIRE(sum[2] == 2);
        REQUIRE(sum[3] == 4);
    }

    SECTION("subtraction") {
        auto diff = zs1 - zs2;
        REQUIRE(diff[1] == 2);
        REQUIRE(diff[2] == 4);
        REQUIRE(diff[3] == -4);
    }

    SECTION("negation") {
        auto neg = -zs1;
        REQUIRE(neg[1] == -2);
        REQUIRE(neg[2] == -3);
    }
}

TEST_CASE("ZSet operations", "[zset]") {
    ZSet<int> zs;

    SECTION("distinct") {
        zs.insert(1, 5);
        zs.insert(2, -3);
        zs.insert(3, 1);

        auto distinct = zs.distinct();
        REQUIRE(distinct[1] == 1);
        REQUIRE(distinct[2] == 0);
        REQUIRE(distinct[3] == 1);
        REQUIRE(distinct.support_size() == 2);
    }

    SECTION("map") {
        zs.insert(1, 2);
        zs.insert(2, 3);
        zs.insert(3, 1);

        auto mapped = zs.map<int>([](int x) { return x * 2; });
        REQUIRE(mapped[2] == 2);
        REQUIRE(mapped[4] == 3);
        REQUIRE(mapped[6] == 1);
    }

    SECTION("filter") {
        zs.insert(1, 1);
        zs.insert(2, 2);
        zs.insert(3, 3);
        zs.insert(4, 4);

        auto filtered = zs.filter([](int x) { return x % 2 == 0; });
        REQUIRE(filtered[1] == 0);
        REQUIRE(filtered[2] == 2);
        REQUIRE(filtered[3] == 0);
        REQUIRE(filtered[4] == 4);
        REQUIRE(filtered.support_size() == 2);
    }
}

TEST_CASE("Stream operators", "[stream]") {
    SECTION("integration") {
        Integration<int> integrate;

        ZSet<int> delta1;
        delta1.insert(1, 1);
        delta1.insert(2, 2);

        auto result1 = integrate.process(delta1);
        REQUIRE(result1[1] == 1);
        REQUIRE(result1[2] == 2);

        ZSet<int> delta2;
        delta2.insert(2, 3);
        delta2.insert(3, 1);

        auto result2 = integrate.process(delta2);
        REQUIRE(result2[1] == 1);
        REQUIRE(result2[2] == 5);
        REQUIRE(result2[3] == 1);
    }

    SECTION("delay") {
        Delay<int> delay;

        ZSet<int> input1;
        input1.insert(1, 1);

        auto output1 = delay.process(input1);
        REQUIRE(output1.empty());

        ZSet<int> input2;
        input2.insert(2, 2);

        auto output2 = delay.process(input2);
        REQUIRE(output2[1] == 1);
        REQUIRE(output2[2] == 0);
    }

    SECTION("incremental distinct") {
        IncrementalDistinct<int> distinct;

        ZSet<int> delta1;
        delta1.insert(1, 2);
        auto result1 = distinct.process(delta1);
        REQUIRE(result1[1] == 1);

        ZSet<int> delta2;
        delta2.insert(1, 1);
        auto result2 = distinct.process(delta2);
        REQUIRE(result2.empty());

        ZSet<int> delta3;
        delta3.insert(1, -3);
        auto result3 = distinct.process(delta3);
        REQUIRE(result3[1] == -1);
    }
}
