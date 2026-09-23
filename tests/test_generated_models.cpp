/**
 * @file       test_generated_models.cpp
 * @brief      Null-guard regression tests for generated model deserialization
 * @date       2026-09-23
 * @author     Kenneth L. Hurley
 *
 * The cpp-pistache template emits no null guards for optional fields, so the
 * documented "field": null representation of an absent optional threw
 * nlohmann::json::type_error from from_json (PR #4 round-2 P2s:
 * OrderCreate.table_id for tableless quick-orders, Location.tax_rate for an
 * unconfigured rate). fix_generated_destructors.py now guards those blocks
 * at generation time — but ONLY for schema-nullable properties (PR #4
 * round-4 P2): a null on a NON-nullable optional (UserUpdate.email) is a
 * client error and must keep throwing so the handler answers 400 instead of
 * silently treating the field as omitted. These tests pin both behaviors
 * against the tracked generated trees.
 */

#include <gtest/gtest.h>

#include "nlohmann/json.hpp"

#include "Location.h"
#include "OrderCreate.h"
#include "UserUpdate.h"

using org::openapitools::server::model::Location;
using org::openapitools::server::model::OrderCreate;
using org::openapitools::server::model::UserUpdate;

TEST(OrderCreateGeneratedModel, NullTableIdLeavesFieldUnset)
{
    const auto parsed = nlohmann::json::parse(R"({
        "status": "open",
        "channel": "pos",
        "fulfillment_type": "pickup",
        "total": {"amount": 1250, "currency": "USD"},
        "table_id": null,
        "customer_id": null
    })");

    OrderCreate order;
    ASSERT_NO_THROW(parsed.get_to(order));
    EXPECT_FALSE(order.tableIdIsSet());
    EXPECT_FALSE(order.customerIdIsSet());
    EXPECT_EQ("open", order.getStatus());
}

TEST(LocationGeneratedModel, NullTaxRateLeavesFieldUnset)
{
    const auto parsed = nlohmann::json::parse(R"({
        "id": "loc-1",
        "tenant_id": "default",
        "organization_id": "default",
        "created_at": "2026-01-01T00:00:00Z",
        "updated_at": "2026-01-01T00:00:00Z",
        "name": "Downtown",
        "type": "restaurant",
        "status": "active",
        "tax_rate": null
    })");

    Location location;
    ASSERT_NO_THROW(parsed.get_to(location));
    EXPECT_FALSE(location.taxRateIsSet());
    EXPECT_EQ("Downtown", location.getName());
}

TEST(UserUpdateGeneratedModel, NullEmailOnNonNullableOptionalThrows)
{
    // email is optional but NOT nullable in identity_openapi.json — an
    // explicit null is a client error, not an omission, so from_json must
    // throw (the handler's error path answers 400) instead of quietly
    // leaving emailIsSet() false.
    const auto parsed = nlohmann::json::parse(R"({
        "email": null
    })");

    UserUpdate update;
    EXPECT_THROW(parsed.get_to(update), nlohmann::json::type_error);
}
