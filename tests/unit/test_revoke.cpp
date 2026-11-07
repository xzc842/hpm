// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
//
// Licensed under the Apache License, Version 2.0.

#include <catch2/catch_test_macros.hpp>
#include <hpm/registry/revoke.h>

using namespace hpm;
using namespace hpm::registry;

TEST_CASE("revoke: normalize serial/fingerprint", "[revoke]") {
    CHECK(normalizeSerial("0000aBcD") == "ABCD");
    CHECK(normalizeSerial("15cb48de0cb5c3baae4c2badfe5f1f81") ==
          "15CB48DE0CB5C3BAAE4C2BADFE5F1F81");
    CHECK(normalizeFingerprint("aa:bb:cc") == "AABBCC");
    CHECK(normalizeFingerprint("AA-BB-CC") == "AABBCC");
}

TEST_CASE("revoke: parse and 7 day validity", "[revoke]") {
    const std::string text = R"({
      "schema": 1,
      "updatedAt": "2026-11-07T06:16:37Z",
      "entries": [{
        "serial": "15CB48DE0CB5C3BAAE4C2BADFE5F1F81",
        "certSha256": "FD:D1:2D:38"
      }]
    })";
    auto parsed = parseRevocation(text);
    REQUIRE(parsed);
    CHECK(parsed->schema == 1);
    CHECK(parsed->entries.size() == 1);
    CHECK(parsed->fresh(7));
    CHECK_FALSE(parsed->fresh(0));

    RevocationList old;
    old.updatedAt = "2020-01-01T00:00:00Z";
    CHECK_FALSE(old.fresh(7));

    RevocationList future;
    future.updatedAt = "2999-01-01T00:00:00Z";
    CHECK_FALSE(future.fresh(7));
}
