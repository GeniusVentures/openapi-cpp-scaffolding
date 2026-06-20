/**
 * @file       test_fnv1a.cpp
 * @brief      Unit tests for compile-time and runtime FNV-1a 64-bit hash
 * @date       2026-05-26
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "singleton/fnv1a.hpp"

using namespace gnus::hash;

// Known FNV-1a 64-bit test vectors (verified against reference implementation)
static constexpr uint64_t kHashEmpty  = 0xcbf29ce484222325ULL;  // Fnv1a("")
static constexpr uint64_t kHash_a     = 0xaf63dc4c8601ec8cULL;  // Fnv1a("a")
static constexpr uint64_t kHash_b     = 0xaf63df4c8601f1a5ULL;  // Fnv1a("b")
static constexpr uint64_t kHash_A     = 0xaf63fc4c860222ecULL;  // Fnv1a("A")
static constexpr uint64_t kHash_1     = 0xaf63ac4c86019afcULL;  // Fnv1a("1")
static constexpr uint64_t kHashFoobar = 0x85944171f73967e8ULL;  // Fnv1a("foobar")
static constexpr uint64_t kHashHello  = 0xa430d84680aabd0bULL;  // Fnv1a("hello")
static constexpr uint64_t kHashHttp   = 0x8874a3cc15dc8285ULL;  // Fnv1a("http")
static constexpr uint64_t kHashTest   = 0xf9e6e6ef197c2b25ULL;  // Fnv1a("test")

// Compile-time computed hashes using the constexpr function
static constexpr uint64_t kCtEmpty  = Fnv1a("");
static constexpr uint64_t kCt_a     = Fnv1a("a");
static constexpr uint64_t kCt_foobar = Fnv1a("foobar");
static constexpr uint64_t kCt_hello  = Fnv1a("hello");

// Real handler names from the HRM domain
static constexpr uint64_t kCtCreateEmployee = Fnv1a("create_employee");
static constexpr uint64_t kCtGetEmployee    = Fnv1a("get_employee");
static constexpr uint64_t kCtCreateTenant   = Fnv1a("create_tenant");
static constexpr uint64_t kCtGetTenant      = Fnv1a("get_tenant");
static constexpr uint64_t kCtHealthCheck    = Fnv1a("health_check");

// ============================================================================
// FNV-1a Empty String
// ============================================================================

TEST(Fnv1aTest, EmptyStringReturnsOffsetBasis)
{
    EXPECT_EQ(Fnv1a(""), kFnvOffsetBasis);
    EXPECT_EQ(Fnv1a(std::string("")), kFnvOffsetBasis);
}

// ============================================================================
// FNV-1a Known Test Vectors
// ============================================================================

TEST(Fnv1aTest, KnownVector_a)
{
    EXPECT_EQ(Fnv1a("a"),       kHash_a);
    EXPECT_EQ(Fnv1a(std::string("a")), kHash_a);
}

TEST(Fnv1aTest, KnownVector_b)
{
    EXPECT_EQ(Fnv1a("b"),       kHash_b);
    EXPECT_EQ(Fnv1a(std::string("b")), kHash_b);
}

TEST(Fnv1aTest, KnownVector_A)
{
    EXPECT_EQ(Fnv1a("A"),       kHash_A);
    EXPECT_EQ(Fnv1a(std::string("A")), kHash_A);
}

TEST(Fnv1aTest, KnownVector_1)
{
    EXPECT_EQ(Fnv1a("1"),       kHash_1);
    EXPECT_EQ(Fnv1a(std::string("1")), kHash_1);
}

TEST(Fnv1aTest, KnownVectorFoobar)
{
    EXPECT_EQ(Fnv1a("foobar"),           kHashFoobar);
    EXPECT_EQ(Fnv1a(std::string("foobar")), kHashFoobar);
}

TEST(Fnv1aTest, KnownVectorHello)
{
    EXPECT_EQ(Fnv1a("hello"),            kHashHello);
    EXPECT_EQ(Fnv1a(std::string("hello")),  kHashHello);
}

TEST(Fnv1aTest, KnownVectorHttp)
{
    EXPECT_EQ(Fnv1a("http"),            kHashHttp);
    EXPECT_EQ(Fnv1a(std::string("http")),  kHashHttp);
}

TEST(Fnv1aTest, KnownVectorTest)
{
    EXPECT_EQ(Fnv1a("test"),            kHashTest);
    EXPECT_EQ(Fnv1a(std::string("test")),  kHashTest);
}

// ============================================================================
// Compile-Time vs Runtime Consistency
// ============================================================================

TEST(Fnv1aTest, ConstexprMatchesRuntime_Empty)
{
    EXPECT_EQ(kCtEmpty, Fnv1a(std::string("")));
}

TEST(Fnv1aTest, ConstexprMatchesRuntime_a)
{
    EXPECT_EQ(kCt_a, Fnv1a(std::string("a")));
}

TEST(Fnv1aTest, ConstexprMatchesRuntime_Foobar)
{
    EXPECT_EQ(kCt_foobar, Fnv1a(std::string("foobar")));
}

TEST(Fnv1aTest, ConstexprMatchesRuntime_Hello)
{
    EXPECT_EQ(kCt_hello, Fnv1a(std::string("hello")));
}

TEST(Fnv1aTest, ConstexprMatchesRuntime_RealHandlerNames)
{
    EXPECT_EQ(kCtCreateEmployee, Fnv1a(std::string("create_employee")));
    EXPECT_EQ(kCtGetEmployee,    Fnv1a(std::string("get_employee")));
    EXPECT_EQ(kCtCreateTenant,   Fnv1a(std::string("create_tenant")));
    EXPECT_EQ(kCtGetTenant,      Fnv1a(std::string("get_tenant")));
    EXPECT_EQ(kCtHealthCheck,    Fnv1a(std::string("health_check")));
}

// ============================================================================
// Determinism & Collision Avoidance
// ============================================================================

TEST(Fnv1aTest, Determinism_SameInputSameOutput)
{
    const std::string input = "genius_ai_boss_plugin_handler";
    uint64_t h1 = Fnv1a(input);
    uint64_t h2 = Fnv1a(input);

    EXPECT_EQ(h1, h2);
}

TEST(Fnv1aTest, DifferentInputsDifferentHashes)
{
    EXPECT_NE(Fnv1a("create_employee"),  Fnv1a("get_employee"));
    EXPECT_NE(Fnv1a("create_employee"),  Fnv1a("update_employee"));
    EXPECT_NE(Fnv1a("create_employee"),  Fnv1a("delete_employee"));
    EXPECT_NE(Fnv1a("create_tenant"),    Fnv1a("get_tenant"));
}

TEST(Fnv1aTest, CaseSensitivity)
{
    EXPECT_NE(Fnv1a("a"), Fnv1a("A"));
    EXPECT_NE(Fnv1a("Test"), Fnv1a("test"));
    EXPECT_NE(Fnv1a("TEST"), Fnv1a("test"));
}

// ============================================================================
// Special / Boundary Cases
// ============================================================================

TEST(Fnv1aTest, NonZeroForNonEmptyStrings)
{
    EXPECT_NE(Fnv1a(" "), 0ULL);
    EXPECT_NE(Fnv1a("!"), 0ULL);
    EXPECT_NE(Fnv1a("\n"), 0ULL);
}

TEST(Fnv1aTest, OffsetBasisNotZero)
{
    EXPECT_NE(kFnvOffsetBasis, 0ULL);
}

TEST(Fnv1aTest, PrimeProperty)
{
    EXPECT_NE(kFnvPrime, 0ULL);
    EXPECT_NE(kFnvPrime, 1ULL);
}
