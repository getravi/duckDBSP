#include "catch.hpp"
#include "dbsp_cdc.hpp"

using namespace dbsp_native;

TEST_CASE("Auto-sync default state", "[auto_cdc][p5]") {
  CDCManager manager;
  REQUIRE(!manager.is_auto_sync_enabled());
}

TEST_CASE("Enable and disable auto-sync", "[auto_cdc][p5]") {
  CDCManager manager;

  manager.enable_auto_sync();
  REQUIRE(manager.is_auto_sync_enabled());

  manager.disable_auto_sync();
  REQUIRE(!manager.is_auto_sync_enabled());
}

TEST_CASE("Reset clears auto-sync", "[auto_cdc][p5]") {
  CDCManager manager;

  manager.enable_auto_sync();
  REQUIRE(manager.is_auto_sync_enabled());

  manager.reset();
  REQUIRE(!manager.is_auto_sync_enabled());
}

TEST_CASE("Auto-sync toggle idempotent", "[auto_cdc][p5]") {
  CDCManager manager;

  // Enable twice is fine
  manager.enable_auto_sync();
  manager.enable_auto_sync();
  REQUIRE(manager.is_auto_sync_enabled());

  // Disable twice is fine
  manager.disable_auto_sync();
  manager.disable_auto_sync();
  REQUIRE(!manager.is_auto_sync_enabled());
}
