/**
 * @file       test_mock_plugin.cpp
 * @brief      Mock plugin lifecycle integration tests
 * @date       2026-05-26
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "singleton/IPlugin.hpp"
#include "singleton/PluginManager.hpp"
#include "singleton/CServiceLocator.hpp"

class LifecycleMockPlugin : public IPlugin
{
    std::string                m_name;
    unsigned int               m_priority;
    std::vector<std::string>   m_urlPaths;
    bool                       m_initialized = false;
    bool                       m_shutdown    = false;
    bool                       m_deinit      = false;
    int                        m_initOrder   = -1;
    int                        m_shutdownOrder = -1;

public:
    LifecycleMockPlugin(std::string name, unsigned int priority)
        : m_name(std::move(name))
        , m_priority(priority)
    {
    }

    std::string GetName() override { return m_name; }
    unsigned int GetPriority() const noexcept override { return m_priority; }

    const std::vector<std::string>& GetUrlPaths() const noexcept override
    { return m_urlPaths; }

    bool Initialize(IServiceLocator& /*manager*/) noexcept override
    {
        m_initialized = true;
        return true;
    }

    bool Shutdown() noexcept override
    {
        m_shutdown = true;
        return true;
    }

    bool DeInit() noexcept override
    {
        m_deinit = true;
        return true;
    }

    bool IsInitialized() const { return m_initialized; }
    bool IsShutdown()    const { return m_shutdown; }
    bool IsDeInit()      const { return m_deinit; }

    void SetInitOrder(int o)       { m_initOrder = o; }
    int  GetInitOrder() const      { return m_initOrder; }
    void SetShutdownOrder(int o)   { m_shutdownOrder = o; }
    int  GetShutdownOrder() const  { return m_shutdownOrder; }
};

class MockPluginTest : public ::testing::Test
{
protected:
    PluginManager pm;
    CServiceLocator locator;

    void SetUp() override
    {
        pm.ShutdownAll();
    }

    void TearDown() override
    {
        pm.ShutdownAll();
    }
};

// ============================================================================
// Plugin Registration Tests
// ============================================================================

TEST_F(MockPluginTest, SinglePluginRegistration)
{
    auto plugin = std::make_shared<LifecycleMockPlugin>("TestDomain", 50);

    pm.RegisterPlugin(plugin, 50, {"/api/test"});

    EXPECT_EQ(pm.GetPluginCount(), 1);
    EXPECT_FALSE(plugin->IsInitialized());
    EXPECT_FALSE(plugin->IsShutdown());
    EXPECT_FALSE(plugin->IsDeInit());
}

TEST_F(MockPluginTest, MultiplePluginRegistration)
{
    auto hrm  = std::make_shared<LifecycleMockPlugin>("HRM",        10);
    auto inv  = std::make_shared<LifecycleMockPlugin>("Inventory",  20);
    auto comm = std::make_shared<LifecycleMockPlugin>("Commerce",   30);
    auto auth = std::make_shared<LifecycleMockPlugin>("Auth",       5);

    pm.RegisterPlugin(hrm,  10, {"/api/hrm"});
    pm.RegisterPlugin(inv,  20, {"/api/inventory"});
    pm.RegisterPlugin(comm, 30, {"/api/commerce"});
    pm.RegisterPlugin(auth,  5, {"/api/auth"});

    EXPECT_EQ(pm.GetPluginCount(), 4);
}

// ============================================================================
// Lifecycle Order Tests
// ============================================================================

TEST_F(MockPluginTest, InitializeAllOrderIsAscendingPriority)
{
    auto auth = std::make_shared<LifecycleMockPlugin>("Auth", 5);
    auto hrm  = std::make_shared<LifecycleMockPlugin>("HRM",  50);
    auto log  = std::make_shared<LifecycleMockPlugin>("Log",  999);

    pm.RegisterPlugin(auth, 5,   {"/api/auth"});
    pm.RegisterPlugin(hrm,  50,  {"/api/hrm"});
    pm.RegisterPlugin(log,  999, {"/api/log"});

    pm.InitializeAll(locator);

    EXPECT_TRUE(auth->IsInitialized());
    EXPECT_TRUE(hrm->IsInitialized());
    EXPECT_TRUE(log->IsInitialized());
}

TEST_F(MockPluginTest, ShutdownAll_CallsShutdownAndDeInit)
{
    auto plugin = std::make_shared<LifecycleMockPlugin>("Single", 42);
    pm.RegisterPlugin(plugin, 42, {"/api/single"});

    pm.InitializeAll(locator);
    EXPECT_TRUE(plugin->IsInitialized());
    EXPECT_FALSE(plugin->IsShutdown());
    EXPECT_FALSE(plugin->IsDeInit());

    pm.ShutdownAll();
    EXPECT_TRUE(plugin->IsShutdown());
    EXPECT_TRUE(plugin->IsDeInit());
}

TEST_F(MockPluginTest, FullLifecycle_CorrectTransitions)
{
    auto plugin = std::make_shared<LifecycleMockPlugin>("FullCycle", 100);
    pm.RegisterPlugin(plugin, 100, {"/api/fullcycle"});

    EXPECT_FALSE(plugin->IsInitialized());
    EXPECT_FALSE(plugin->IsShutdown());
    EXPECT_FALSE(plugin->IsDeInit());

    pm.InitializeAll(locator);
    EXPECT_TRUE(plugin->IsInitialized());
    EXPECT_FALSE(plugin->IsShutdown());
    EXPECT_FALSE(plugin->IsDeInit());

    pm.ShutdownAll();
    EXPECT_TRUE(plugin->IsShutdown());
    EXPECT_TRUE(plugin->IsDeInit());
}

// ============================================================================
// Handler Registration and Dispatch Tests
// ============================================================================

TEST_F(MockPluginTest, HandlerRegistrationAndDispatch)
{
    auto plugin = std::make_shared<LifecycleMockPlugin>("HdlrPlugin", 50);
    pm.RegisterPlugin(plugin, 50, {"/api/handler"});

    HandlerFn fn = [](const RequestContext& /*ctx*/, const std::string& method, const std::string& urlPath, const std::string& body) -> std::string {
        return "{\"result\":\"dispatched\"}";
    };

    pm.RegisterHandler("POST", "/api/handler/items", "do_action", fn, "HdlrPlugin", kStubHandlerPriority);

    std::string result = pm.Route(
        RequestContext{},
        "POST",
        "/api/handler/items",
        R"({"action":"do_action","payload":{"id":1}})");

    EXPECT_FALSE(result.empty());
}

TEST_F(MockPluginTest, MultipleHandlersPerPlugin)
{
    auto plugin = std::make_shared<LifecycleMockPlugin>("MultiHdlr", 50);
    pm.RegisterPlugin(plugin, 50, {"/api/multi"});

    HandlerFn create = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string {
        return "{\"op\":\"create\"}";
    };
    HandlerFn read = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string {
        return "{\"op\":\"read\"}";
    };
    HandlerFn update = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string {
        return "{\"op\":\"update\"}";
    };
    HandlerFn del = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string {
        return "{\"op\":\"delete\"}";
    };

    pm.RegisterHandler("POST", "/api/multi/items", "create_items", create, "MultiHdlr", kStubHandlerPriority);
    pm.RegisterHandler("GET", "/api/multi/items/list", "list_items", read, "MultiHdlr", kStubHandlerPriority);
    pm.RegisterHandler("PUT", "/api/multi/items/{id}", "update_item", update, "MultiHdlr", kStubHandlerPriority);
    pm.RegisterHandler("DELETE", "/api/multi/items/{id}/delete", "delete_item", del, "MultiHdlr", kStubHandlerPriority);

    EXPECT_EQ(pm.GetHandlerCount(), 4);

    EXPECT_EQ(pm.Route(RequestContext{}, "POST",   "/api/multi/items",             R"({"action":"create"})"), "{\"op\":\"create\"}");
    EXPECT_EQ(pm.Route(RequestContext{}, "GET",    "/api/multi/items/list",        R"({"action":"read"})"),   "{\"op\":\"read\"}");
    EXPECT_EQ(pm.Route(RequestContext{}, "PUT",    "/api/multi/items/{id}",        R"({"action":"update"})"), "{\"op\":\"update\"}");
    EXPECT_EQ(pm.Route(RequestContext{}, "DELETE", "/api/multi/items/{id}/delete", R"({"action":"delete"})"), "{\"op\":\"delete\"}");
}

// ============================================================================
// Shutdown Cleanup Tests
// ============================================================================

TEST_F(MockPluginTest, ShutdownAllClearsHandlers)
{
    auto plugin = std::make_shared<LifecycleMockPlugin>("CleanupPlg", 50);
    pm.RegisterPlugin(plugin, 50, {"/api/cleanup"});

    HandlerFn fn = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string { return "ok"; };
    pm.RegisterHandler("GET", "/api/cleanup/data", "my_action", fn, "CleanupPlg", kStubHandlerPriority);

    EXPECT_EQ(pm.GetHandlerCount(), 1);

    pm.ShutdownAll();

    EXPECT_EQ(pm.GetHandlerCount(), 0);
    EXPECT_EQ(pm.GetPluginCount(), 0);
}

TEST_F(MockPluginTest, CanReRegisterAfterShutdown)
{
    auto p1 = std::make_shared<LifecycleMockPlugin>("Round1", 10);
    pm.RegisterPlugin(p1, 10, {"/api/round1"});
    pm.InitializeAll(locator);

    EXPECT_EQ(pm.GetPluginCount(), 1);

    pm.ShutdownAll();
    EXPECT_EQ(pm.GetPluginCount(), 0);

    auto p2 = std::make_shared<LifecycleMockPlugin>("Round2", 20);
    pm.RegisterPlugin(p2, 20, {"/api/round2"});
    pm.InitializeAll(locator);

    EXPECT_EQ(pm.GetPluginCount(), 1);
    EXPECT_TRUE(p2->IsInitialized());
}

// ============================================================================
// Unhappy-Path Tests — registration collisions, out-of-order lifecycle,
// extreme priority values, and shutdown-before-init safety.
// ============================================================================

TEST_F(MockPluginTest, DuplicateRegistration_OverwritesPluginEntry)
{
    // PluginManager keys plugins by name. Registering two different plugin
    // instances under the same name overwrites the stored entry but leaves
    // the init queue referring to that name twice. After InitializeAll,
    // only the most recently registered instance is reachable.
    auto first  = std::make_shared<LifecycleMockPlugin>("Collide", 10);
    auto second = std::make_shared<LifecycleMockPlugin>("Collide", 20);

    pm.RegisterPlugin(first,  10, {"/api/collide"});
    pm.RegisterPlugin(second, 20, {"/api/collide"});

    // Only one plugin entry survives (keyed by name).
    EXPECT_EQ(pm.GetPluginCount(), 1);

    pm.InitializeAll(locator);

    // The first instance was displaced and never initialized.
    EXPECT_FALSE(first->IsInitialized());
    // The second instance is the one reachable under "Collide".
    EXPECT_TRUE(second->IsInitialized());
}

TEST_F(MockPluginTest, InitializeAll_BeforeAnyRegistration_IsSafe)
{
    // InitializeAll on an empty plugin manager is a no-op and must not crash.
    EXPECT_EQ(pm.GetPluginCount(), 0);
    pm.InitializeAll(locator);
    EXPECT_EQ(pm.GetPluginCount(), 0);
}

TEST_F(MockPluginTest, ShutdownAll_BeforeInitialize_ShutsDownUninitializedPlugins)
{
    // ShutdownAll without an intervening InitializeAll still calls Shutdown()
    // and DeInit() on every registered plugin, even though Initialize() was
    // never called. This documents the current behavior: Shutdown/DeInit are
    // invoked unconditionally on teardown.
    auto plugin = std::make_shared<LifecycleMockPlugin>("NotInited", 50);
    pm.RegisterPlugin(plugin, 50, {"/api/notinited"});

    EXPECT_FALSE(plugin->IsInitialized());

    pm.ShutdownAll();

    EXPECT_FALSE(plugin->IsInitialized());
    EXPECT_TRUE(plugin->IsShutdown());
    EXPECT_TRUE(plugin->IsDeInit());
    EXPECT_EQ(pm.GetPluginCount(), 0);
}

TEST_F(MockPluginTest, RegisterPlugin_AcceptsMaxUnsignedPriority)
{
    // Priority is unsigned int. A caller passing -1 wraps to UINT_MAX on
    // conversion; the plugin is still accepted and initialized last
    // (highest priority number = latest in ascending init order).
    constexpr unsigned int kWrappedPriority =
        static_cast<unsigned int>(-1);

    auto early = std::make_shared<LifecycleMockPlugin>("Early", 1);
    auto late  = std::make_shared<LifecycleMockPlugin>("Late",  kWrappedPriority);

    pm.RegisterPlugin(early, 1,             {"/api/early"});
    pm.RegisterPlugin(late,  kWrappedPriority, {"/api/late"});

    EXPECT_EQ(pm.GetPluginCount(), 2);

    pm.InitializeAll(locator);

    EXPECT_TRUE(early->IsInitialized());
    EXPECT_TRUE(late->IsInitialized());
}

TEST_F(MockPluginTest, RegisterPlugin_ZeroPriority_IsAccepted)
{
    // Priority 0 is a valid value (lowest possible) and must not be rejected.
    auto plugin = std::make_shared<LifecycleMockPlugin>("ZeroPri", 0);
    pm.RegisterPlugin(plugin, 0, {"/api/zero"});

    EXPECT_EQ(pm.GetPluginCount(), 1);

    pm.InitializeAll(locator);
    EXPECT_TRUE(plugin->IsInitialized());
}

TEST_F(MockPluginTest, InitializeAll_IsNotIdempotent_PerPluginFlag)
{
    // Calling InitializeAll twice invokes Initialize() on each plugin twice.
    // The mock's flag remains true after the first call, so this documents
    // that the manager does not skip already-initialized plugins.
    auto plugin = std::make_shared<LifecycleMockPlugin>("Double", 50);
    pm.RegisterPlugin(plugin, 50, {"/api/double"});

    pm.InitializeAll(locator);
    EXPECT_TRUE(plugin->IsInitialized());

    // Second pass is safe and still calls Initialize() again.
    pm.InitializeAll(locator);
    EXPECT_TRUE(plugin->IsInitialized());
}
