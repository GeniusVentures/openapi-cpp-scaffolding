/**
 * @file       test_plugin_manager.cpp
 * @brief      Unit tests for PluginManager — registration, routing, lifecycle
 * @date       2026-05-26
 */
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "singleton/IPlugin.hpp"
#include "singleton/PluginManager.hpp"
#include "singleton/CServiceLocator.hpp"

// ============================================================================
// Mock Plugin — minimal IPlugin implementation for testing
// ============================================================================

class MockPlugin : public IPlugin
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
    explicit MockPlugin(std::string name, unsigned int priority = 100)
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

    void SetInitOrder(int order)    { m_initOrder = order; }
    int  GetInitOrder() const       { return m_initOrder; }
    void SetShutdownOrder(int order) { m_shutdownOrder = order; }
    int  GetShutdownOrder() const   { return m_shutdownOrder; }
};

// ============================================================================
// Test Fixture — ensures clean PluginManager state before each test
// ============================================================================

class PluginManagerTest : public ::testing::Test
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

    static std::shared_ptr<MockPlugin> MakeMock(
        const std::string& name, unsigned int priority = 100)
    {
        return std::make_shared<MockPlugin>(name, priority);
    }
};

// ============================================================================
// RegisterPlugin Tests
// ============================================================================

TEST_F(PluginManagerTest, RegisterPlugin_IncrementsCount)
{
    EXPECT_EQ(pm.GetPluginCount(), 0);

    auto p1 = MakeMock("TestPlugin");
    pm.RegisterPlugin(p1, 100, {"/api/test"});
    EXPECT_EQ(pm.GetPluginCount(), 1);

    auto p2 = MakeMock("OtherPlugin");
    pm.RegisterPlugin(p2, 200, {"/api/other"});
    EXPECT_EQ(pm.GetPluginCount(), 2);
}

TEST_F(PluginManagerTest, RegisterPlugin_SetsUrlRoutes)
{
    auto plugin = MakeMock("HrmPlugin");
    std::vector<std::string> paths = {"/api/hrm", "/api/employees"};
    pm.RegisterPlugin(plugin, 100, paths);

    auto plugin2 = MakeMock("HrmPlugin");
    pm.RegisterPlugin(plugin2, 100, {"/api/hrm/v2"});

    EXPECT_EQ(pm.GetPluginCount(), 1);
}

TEST_F(PluginManagerTest, RegisterPlugin_MultiplePathsPerPlugin)
{
    auto plugin = MakeMock("MultiPathPlugin");
    pm.RegisterPlugin(plugin, 50, {"/api/foo", "/api/bar", "/api/baz"});

    EXPECT_EQ(pm.GetPluginCount(), 1);
}

// ============================================================================
// RegisterHandler Tests
// ============================================================================

TEST_F(PluginManagerTest, RegisterHandler_IncrementsCount)
{
    EXPECT_EQ(pm.GetHandlerCount(), 0);

    auto plugin = MakeMock("HandlerPlugin");
    pm.RegisterPlugin(plugin, 100, {"/api/handlers"});

    HandlerFn fn = [](const RequestContext& /*ctx*/, const std::string& method, const std::string& urlPath, const std::string& body) -> std::string {
        return "{\"result\":\"ok\"}";
    };

    pm.RegisterHandler("POST", "/api/handlers/create", "create_item", fn, "HandlerPlugin", kStubHandlerPriority);
    EXPECT_EQ(pm.GetHandlerCount(), 1);

    pm.RegisterHandler("GET", "/api/handlers/list", "list_items", fn, "HandlerPlugin", kStubHandlerPriority);
    EXPECT_EQ(pm.GetHandlerCount(), 2);
}

TEST_F(PluginManagerTest, RegisterHandler_SamePathSamePluginOverwrites)
{
    auto plugin = MakeMock("OverwritePlugin");
    pm.RegisterPlugin(plugin, 100, {"/api/overwrite"});

    HandlerFn fn1 = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string { return "first"; };
    HandlerFn fn2 = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string { return "second"; };

    pm.RegisterHandler("POST", "/api/overwrite/data", "do_thing", fn1, "OverwritePlugin", kStubHandlerPriority);
    pm.RegisterHandler("POST", "/api/overwrite/data", "do_thing", fn2, "OverwritePlugin", kStubHandlerPriority);

    EXPECT_EQ(pm.GetHandlerCount(), 1);
    EXPECT_EQ(pm.Route(RequestContext{}, "POST", "/api/overwrite/data", "{}"), "second");
}

TEST_F(PluginManagerTest, RegisterHandler_SamePathDifferentPriority)
{
    auto plugin = MakeMock("PriorityPlugin");
    pm.RegisterPlugin(plugin, 100, {"/api/pri"});

    HandlerFn fn1 = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string { return "v1"; };
    HandlerFn fn2 = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string { return "v2"; };

    pm.RegisterHandler("GET", "/api/pri/data", "do_thing_v1", fn1, "PriorityPlugin", 100);
    pm.RegisterHandler("GET", "/api/pri/data", "do_thing_v2", fn2, "PriorityPlugin", 200);

    EXPECT_EQ(pm.GetHandlerCount(), 2);
    EXPECT_EQ(pm.Route(RequestContext{}, "GET", "/api/pri/data", "{}"), "v2");
}

// ============================================================================
// Priority Ordering Tests
// ============================================================================

TEST_F(PluginManagerTest, PriorityOrder_InitializeAllAscending)
{
    auto p10   = MakeMock("Priority10",   10);
    auto p50   = MakeMock("Priority50",   50);
    auto p100  = MakeMock("Priority100", 100);
    auto p999  = MakeMock("Priority999", 999);

    pm.RegisterPlugin(p10,   10,  {"/api/10"});
    pm.RegisterPlugin(p50,   50,  {"/api/50"});
    pm.RegisterPlugin(p100, 100, {"/api/100"});
    pm.RegisterPlugin(p999, 999, {"/api/999"});

    EXPECT_FALSE(p10->IsInitialized());
    EXPECT_FALSE(p50->IsInitialized());
    EXPECT_FALSE(p100->IsInitialized());
    EXPECT_FALSE(p999->IsInitialized());

    pm.InitializeAll(locator);

    EXPECT_TRUE(p10->IsInitialized());
    EXPECT_TRUE(p50->IsInitialized());
    EXPECT_TRUE(p100->IsInitialized());
    EXPECT_TRUE(p999->IsInitialized());
}

TEST_F(PluginManagerTest, PriorityOrder_ShutdownAllReverse)
{
    auto p10  = MakeMock("Shutdown10",   10);
    auto p50  = MakeMock("Shutdown50",   50);
    auto p100 = MakeMock("Shutdown100", 100);

    pm.RegisterPlugin(p10,   10,  {"/api/s10"});
    pm.RegisterPlugin(p50,   50,  {"/api/s50"});
    pm.RegisterPlugin(p100, 100, {"/api/s100"});

    pm.InitializeAll(locator);
    pm.ShutdownAll();

    EXPECT_TRUE(p10->IsShutdown());
    EXPECT_TRUE(p10->IsDeInit());
    EXPECT_TRUE(p50->IsShutdown());
    EXPECT_TRUE(p50->IsDeInit());
    EXPECT_TRUE(p100->IsShutdown());
    EXPECT_TRUE(p100->IsDeInit());
}

TEST_F(PluginManagerTest, PriorityOrder_SamePriorityHandling)
{
    auto pa = MakeMock("SamePriA", 50);
    auto pb = MakeMock("SamePriB", 50);
    auto pc = MakeMock("SamePriC", 50);

    pm.RegisterPlugin(pa, 50, {"/api/a"});
    pm.RegisterPlugin(pb, 50, {"/api/b"});
    pm.RegisterPlugin(pc, 50, {"/api/c"});

    EXPECT_EQ(pm.GetPluginCount(), 3);

    pm.InitializeAll(locator);

    EXPECT_TRUE(pa->IsInitialized());
    EXPECT_TRUE(pb->IsInitialized());
    EXPECT_TRUE(pc->IsInitialized());
}

// ============================================================================
// Route Tests — Path-Based Dispatch
// ============================================================================

TEST_F(PluginManagerTest, Route_DirectPathMatch)
{
    auto plugin = MakeMock("HrmPlugin");
    pm.RegisterPlugin(plugin, 100, {"/api/hrm"});

    HandlerFn fn = [](const RequestContext& /*ctx*/, const std::string& method, const std::string& urlPath, const std::string& body) -> std::string {
        return "{\"status\":\"handled\"}";
    };

    pm.RegisterHandler("POST", "/api/hrm/employees", "create_employee", fn, "HrmPlugin", kStubHandlerPriority);

    std::string result = pm.Route(
        RequestContext{},
        "POST",
        "/api/hrm/employees",
        R"({"action":"create_employee","payload":{"name":"test"}})");

    EXPECT_FALSE(result.empty());
    EXPECT_EQ(result, "{\"status\":\"handled\"}");
}

TEST_F(PluginManagerTest, Route_MultiplePluginsDifferentPaths)
{
    auto commerce  = MakeMock("CommercePlugin");
    auto inventory = MakeMock("InventoryPlugin");
    auto events    = MakeMock("EventsPlugin");

    pm.RegisterPlugin(commerce,  100, {"/api/commerce"});
    pm.RegisterPlugin(inventory, 100, {"/api/inventory"});
    pm.RegisterPlugin(events,    100, {"/api/events"});

    HandlerFn cf  = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string { return "commerce"; };
    HandlerFn itf = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string { return "inventory"; };
    HandlerFn ef  = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string { return "events"; };

    pm.RegisterHandler("GET", "/api/commerce/products", "get_products", cf, "CommercePlugin", kStubHandlerPriority);
    pm.RegisterHandler("GET", "/api/inventory/items", "get_items", itf, "InventoryPlugin", kStubHandlerPriority);
    pm.RegisterHandler("GET", "/api/inventory/items/{id}", "get_item",       itf, "InventoryPlugin", kStubHandlerPriority);
    pm.RegisterHandler("GET", "/api/events/recent", "get_recent", ef, "EventsPlugin", kStubHandlerPriority);

    EXPECT_EQ(pm.Route(RequestContext{}, "GET", "/api/commerce/products",   R"({"action":"get"})"), "commerce");
    EXPECT_EQ(pm.Route(RequestContext{}, "GET", "/api/inventory/items",      R"({"action":"get"})"), "inventory");
    EXPECT_EQ(pm.Route(RequestContext{}, "GET", "/api/inventory/items/{id}", R"({"action":"get"})"), "inventory");
    EXPECT_EQ(pm.Route(RequestContext{}, "GET", "/api/events/recent",        R"({"action":"get"})"), "events");
}

TEST_F(PluginManagerTest, Route_NoMatchingRouteReturnsEmpty)
{
    std::string result = pm.Route(RequestContext{}, "GET", "/api/nonexistent", R"({"action":"foo"})");
    EXPECT_TRUE(result.empty());
}

TEST_F(PluginManagerTest, Route_EmptyBodyStillDispatches)
{
    auto plugin = MakeMock("EmptyBodyPlugin");
    pm.RegisterPlugin(plugin, 100, {"/api/empty"});

    HandlerFn fn = [](const RequestContext& /*ctx*/, const std::string& method, const std::string& urlPath, const std::string& body) -> std::string {
        return "{\"got\":\"" + body + "\"}";
    };
    pm.RegisterHandler("GET", "/api/empty/data", "get_data", fn, "EmptyBodyPlugin", kStubHandlerPriority);

    std::string result = pm.Route(RequestContext{}, "GET", "/api/empty/data", "");
    EXPECT_EQ(result, "{\"got\":\"\"}");
}

TEST_F(PluginManagerTest, Route_HighestPriorityHandlerWins)
{
    auto v1Plugin = MakeMock("V1Plugin", 100);
    auto v2Plugin = MakeMock("V2Plugin", 200);

    pm.RegisterPlugin(v1Plugin, 100, {"/api/svc"});
    pm.RegisterPlugin(v2Plugin, 200, {"/api/svc"});

    HandlerFn v1 = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string { return "v1"; };
    HandlerFn v2 = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string { return "v2"; };

    pm.RegisterHandler("GET", "/api/svc/data", "get_data", v1, "V1Plugin", 100);
    pm.RegisterHandler("GET", "/api/svc/data", "get_data", v2, "V2Plugin", 200);

    EXPECT_EQ(pm.Route(RequestContext{}, "GET", "/api/svc/data", "{}"), "v2");
}

TEST_F(PluginManagerTest, Route_HandlerReceivesJsonBody)
{
    auto plugin = MakeMock("EchoPlugin");
    pm.RegisterPlugin(plugin, 100, {"/api/echo"});

    HandlerFn fn = [](const RequestContext& /*ctx*/, const std::string& method, const std::string& urlPath, const std::string& body) -> std::string { return body; };
    pm.RegisterHandler("POST", "/api/echo/reflect", "reflect", fn, "EchoPlugin", kStubHandlerPriority);

    std::string payload = R"({"action":"reflect","data":{"key":"value"}})";
    EXPECT_EQ(pm.Route(RequestContext{}, "POST", "/api/echo/reflect", payload), payload);
}

// ============================================================================
// ShutdownAll Tests
// ============================================================================

TEST_F(PluginManagerTest, ShutdownAll_ClearsAllMaps)
{
    auto plugin = MakeMock("ClearPlugin");
    pm.RegisterPlugin(plugin, 100, {"/api/clear"});

    HandlerFn fn = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string { return "ok"; };
    pm.RegisterHandler("GET", "/api/clear/data", "do_stuff", fn, "ClearPlugin", kStubHandlerPriority);

    EXPECT_EQ(pm.GetPluginCount(), 1);
    EXPECT_EQ(pm.GetHandlerCount(), 1);

    pm.ShutdownAll();

    EXPECT_EQ(pm.GetPluginCount(), 0);
    EXPECT_EQ(pm.GetHandlerCount(), 0);
}

TEST_F(PluginManagerTest, ShutdownAll_Idempotent)
{
    auto plugin = MakeMock("IdempotentPlugin");
    pm.RegisterPlugin(plugin, 100, {"/api/idem"});

    pm.ShutdownAll();
    EXPECT_EQ(pm.GetPluginCount(), 0);

    pm.ShutdownAll();
    EXPECT_EQ(pm.GetPluginCount(), 0);

    pm.ShutdownAll();
    EXPECT_EQ(pm.GetPluginCount(), 0);
}

// ============================================================================
// Accessor Tests
// ============================================================================

TEST_F(PluginManagerTest, GetPluginCount_ReturnsCorrectCount)
{
    EXPECT_EQ(pm.GetPluginCount(), 0);

    pm.RegisterPlugin(MakeMock("P1"), 10, {"/api/p1"});
    EXPECT_EQ(pm.GetPluginCount(), 1);

    pm.RegisterPlugin(MakeMock("P2"), 20, {"/api/p2"});
    EXPECT_EQ(pm.GetPluginCount(), 2);
}

TEST_F(PluginManagerTest, GetHandlerCount_ReturnsCorrectCount)
{
    pm.RegisterPlugin(MakeMock("CountPlugin"), 10, {"/api/count"});

    EXPECT_EQ(pm.GetHandlerCount(), 0);

    HandlerFn fn = [](const RequestContext& /*ctx*/, const std::string&, const std::string&, const std::string&) -> std::string { return "ok"; };
    pm.RegisterHandler("GET", "/api/count/a1", "a1", fn, "CountPlugin", kStubHandlerPriority);
    pm.RegisterHandler("GET", "/api/count/a2", "a2", fn, "CountPlugin", kStubHandlerPriority);
    pm.RegisterHandler("GET", "/api/count/a3", "a3", fn, "CountPlugin", kStubHandlerPriority);

    EXPECT_EQ(pm.GetHandlerCount(), 3);
}

// ============================================================================
// Registration Order Tests
// ============================================================================

TEST_F(PluginManagerTest, RegistrationOrder_TracksOrder)
{
    pm.RegisterPlugin(MakeMock("First"),  10, {"/api/first"});
    pm.RegisterPlugin(MakeMock("Second"), 20, {"/api/second"});
    pm.RegisterPlugin(MakeMock("Third"),  30, {"/api/third"});

    EXPECT_EQ(pm.GetPluginCount(), 3);
}

TEST_F(PluginManagerTest, MultiplePluginsDifferentPathsWorkTogether)
{
    auto hrm  = MakeMock("Hrm");
    auto inv  = MakeMock("Inventory");
    auto comm = MakeMock("Commerce");

    pm.RegisterPlugin(hrm,  10, {"/api/hrm"});
    pm.RegisterPlugin(inv,  20, {"/api/inventory"});
    pm.RegisterPlugin(comm, 30, {"/api/commerce"});

    EXPECT_EQ(pm.GetPluginCount(), 3);
}
