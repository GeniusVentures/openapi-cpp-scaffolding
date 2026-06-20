#!/usr/bin/env python3
"""
Generate a PluginManager-compatible OOP plugin from an OpenAPI 3.1 JSON spec.

Usage:
    python3 generate_plugin.py <spec.json> <output.cpp>

Emits TWO files per domain:
  1. <output>.hpp  — inline class definition (instance members, virtual
                     handler methods, overridable RegisterStubHandlers).
  2. <output>.cpp  — 3-line export shim: #include of the .hpp + EXPORT_PLUGIN.

The generated plugin class derives from IPlugin. Handler stubs are protected
virtual instance methods. Storage is a protected instance member set during
Initialize(). Initialize() delegates handler registration to a protected
virtual RegisterStubHandlers(PluginManager*) so derived classes can override
individual handlers or the registration order without text-mangling the
generated file.

Plugins register via IServiceLocator — no singletons, no void* context.
Generated handlers perform real CRUD operations against IStorageEngine.
"""

import json
import os
import re
import sys


def slugify(name: str) -> str:
    """Convert a name to a valid C++ identifier."""
    s = re.sub(r'[^a-zA-Z0-9_]', '_', name)
    if s and s[0].isdigit():
        s = '_' + s
    return s


def pascal_case(slug: str) -> str:
    """Return the slug with the first character uppercased.

    'hrm'      -> 'Hrm'
    'identity' -> 'Identity'
    Used to build the plugin class name (e.g. 'HrmPlugin').
    """
    if not slug:
        return slug
    return slug[0].upper() + slug[1:]


def extract_entity(path: str) -> str:
    """Extract the entity name from a URL path.

    For /api/v1/employees          -> 'employees'
    For /api/v1/employees/{id}     -> 'employees'
    For /api/v1/time-entries       -> 'time-entries'
    For /api/v1/time-entries/{id}  -> 'time-entries'
    """
    segments = [s for s in path.split('/') if s]
    # Find the last non-parameter segment
    for seg in reversed(segments):
        if not seg.startswith('{'):
            return seg
    return segments[-1] if segments else 'unknown'


def classify_operation(method: str, path: str) -> str:
    """Classify an HTTP method + path into a CRUD operation type.

    Returns: 'list', 'create', 'get', 'update', or 'delete'
    """
    has_param = '{' in path
    if method == 'GET' and not has_param:
        return 'list'
    if method == 'POST' and not has_param:
        return 'create'
    if method == 'GET' and has_param:
        return 'get'
    if method == 'PATCH' and has_param:
        return 'update'
    if method == 'DELETE' and has_param:
        return 'delete'
    # Fallback for PUT
    if method == 'PUT' and has_param:
        return 'update'
    return 'unknown'


def generate_handler_method(operation_id: str, method: str, path: str,
                            domain: str, entity: str) -> str:
    """Generate a CRUD handler instance method for an endpoint.

    Returns the body of a protected virtual instance method (no leading
    'static'). Storage access is via the m_storage instance member.
    """
    op_type = classify_operation(method, path)

    if op_type == 'create':
        return _generate_create_handler(operation_id, domain, entity)
    elif op_type == 'list':
        return _generate_list_handler(operation_id, domain, entity)
    elif op_type == 'get':
        return _generate_get_handler(operation_id, domain, entity)
    elif op_type == 'update':
        return _generate_update_handler(operation_id, domain, entity)
    elif op_type == 'delete':
        return _generate_delete_handler(operation_id, domain, entity)
    else:
        return _generate_fallback_handler(operation_id)


# Handler method signature templates. Non-static, virtual, full parameter names.
# Create handlers use `ctx` to stamp tenant_id / organization_id from the JWT;
# all others mark ctx as unused.
_HANDLER_SIGNATURE = '''    virtual std::string {op}(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)'''

_HANDLER_SIGNATURE_CREATE = '''    virtual std::string {op}(
        const RequestContext& ctx,
        const std::string& /*method*/,
        const std::string& /*urlPath*/,
        const std::string& body)'''


def _handler_decl(operation_id: str) -> str:
    """Return the method declaration line (no body)."""
    return _HANDLER_SIGNATURE.format(op=operation_id)


def _handler_decl_create(operation_id: str) -> str:
    """Return the method declaration line for create handlers (uses ctx)."""
    return _HANDLER_SIGNATURE_CREATE.format(op=operation_id)


def _generate_create_handler(operation_id: str, domain: str, entity: str) -> str:
    decl = _handler_decl_create(operation_id)
    return f'''///
/// CRUD handler for {operation_id} — create a new {entity} entity
///
{decl}
    {{
        try
        {{
            json requestData = json::parse(body);
            std::string id = GenerateUuid();
            requestData["id"] = id;
            requestData["tenant_id"] = ctx.tenantId;
            requestData["organization_id"] = ctx.organizationId;
            requestData["created_at"] = GetCurrentTimestamp();
            requestData["updated_at"] = requestData["created_at"];

            auto keyResult = KeyBuilder::Build("{domain}", "{entity}", id);
            if (!keyResult.has_value())
            {{
                return R"({{"error":{{"code":"INVALID_KEY","message":"Failed to build storage key"}}}})";
            }}
            std::string key = keyResult.value();

            if (!m_storage->Put(key, requestData.dump()))
            {{
                return R"({{"error":{{"code":"STORAGE_ERROR","message":"Failed to store entity"}}}})";
            }}
            return requestData.dump();
        }}
        catch (const json::parse_error&)
        {{
            return R"({{"error":{{"code":"PARSE_ERROR","message":"Invalid JSON body"}}}})";
        }}
    }}'''


def _generate_list_handler(operation_id: str, domain: str, entity: str) -> str:
    decl = _handler_decl(operation_id)
    return f'''///
/// CRUD handler for {operation_id} — list {entity} entities
///
{decl}
    {{
        std::string prefix = KeyBuilder::MakePrefix("{domain}", "{entity}");
        auto scanResult = m_storage->Scan(prefix);

        json result = json::array();
        for (const auto& [k, v] : scanResult)
        {{
            json item = json::parse(v);
            // Backfill multi-tenant fields for records created before
            // tenant stamping was added to create handlers.
            if (!item.contains("tenant_id"))
            {{
                item["tenant_id"] = "default";
            }}
            if (!item.contains("organization_id"))
            {{
                item["organization_id"] = "default";
            }}
            result.push_back(item);
        }}
        json response;
        response["data"] = result;
        json pagination;
        pagination["limit"] = kDefaultPaginationLimit;
        pagination["has_more"] = false;
        response["pagination"] = pagination;
        return response.dump();
    }}'''


def _generate_get_handler(operation_id: str, domain: str, entity: str) -> str:
    decl = _handler_decl(operation_id)
    return f'''///
/// CRUD handler for {operation_id} — get a single {entity} entity
///
{decl}
    {{
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("{domain}", "{entity}", id);
        if (!keyResult.has_value())
        {{
            return R"({{"error":{{"code":"INVALID_KEY","message":"Invalid entity ID"}}}})";
        }}
        std::string key = keyResult.value();

        std::string value;
        if (!m_storage->Get(key, value))
        {{
            return R"({{"error":{{"code":"NOT_FOUND","message":"Entity not found"}}}})";
        }}
        json item = json::parse(value);
        if (!item.contains("tenant_id")) {{ item["tenant_id"] = "default"; }}
        if (!item.contains("organization_id")) {{ item["organization_id"] = "default"; }}
        return item.dump();
    }}'''


def _generate_update_handler(operation_id: str, domain: str, entity: str) -> str:
    decl = _handler_decl(operation_id)
    return f'''///
/// CRUD handler for {operation_id} — update an existing {entity} entity
///
{decl}
    {{
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("{domain}", "{entity}", id);
        if (!keyResult.has_value())
        {{
            return R"({{"error":{{"code":"INVALID_KEY","message":"Invalid entity ID"}}}})";
        }}
        std::string key = keyResult.value();

        std::string existing;
        if (!m_storage->Get(key, existing))
        {{
            return R"({{"error":{{"code":"NOT_FOUND","message":"Entity not found"}}}})";
        }}

        try
        {{
            json updated = json::parse(existing);
            updated.update(json::parse(body));
            updated["updated_at"] = GetCurrentTimestamp();

            if (!m_storage->Put(key, updated.dump()))
            {{
                return R"({{"error":{{"code":"STORAGE_ERROR","message":"Failed to update entity"}}}})";
            }}
            return updated.dump();
        }}
        catch (const json::parse_error&)
        {{
            return R"({{"error":{{"code":"PARSE_ERROR","message":"Invalid JSON body"}}}})";
        }}
    }}'''


def _generate_delete_handler(operation_id: str, domain: str, entity: str) -> str:
    decl = _handler_decl(operation_id)
    return f'''///
/// CRUD handler for {operation_id} — delete a {entity} entity
///
{decl}
    {{
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("{domain}", "{entity}", id);
        if (!keyResult.has_value())
        {{
            return R"({{"error":{{"code":"INVALID_KEY","message":"Invalid entity ID"}}}})";
        }}
        std::string key = keyResult.value();

        if (!m_storage->Delete(key))
        {{
            return R"({{"error":{{"code":"STORAGE_ERROR","message":"Failed to delete entity"}}}})";
        }}
        return R"({{"deleted":true}})";
    }}'''


def _generate_fallback_handler(operation_id: str) -> str:
    decl = _handler_decl(operation_id)
    return f'''///
/// Fallback handler for {operation_id} — operation type not recognized
///
{decl}
    {{
        return R"({{"error":{{"code":"INVALID_PATH","message":"Operation not supported"}}}})";
    }}'''


def _generate_register_stub_handlers(operations):
    """Emit the protected virtual RegisterStubHandlers(PluginManager*) body.

    One lambda-wrapped pm->RegisterHandler(...) call per handler, in sorted
    operation order. Uses kStubHandlerPriority (direct constexpr from
    PluginManager.hpp — no macro).
    """
    lines = []
    lines.append('    virtual void RegisterStubHandlers(PluginManager* pm)')
    lines.append('    {')
    for op in operations:
        op_id = op['operationId']
        lines.append(
            f'        pm->RegisterHandler("{op["method"]}", "{op["path"]}", '
            f'"{op_id}",')
        lines.append(
            f'            [this](const RequestContext& ctx, const std::string& m, '
            f'const std::string& p, const std::string& b) {{')
        lines.append(f'                return {op_id}(ctx, m, p, b);')
        lines.append(f'            }}, GetName(), kStubHandlerPriority);')
    lines.append('    }')
    return '\n'.join(lines)


def generate(spec_path: str, output_path: str):
    with open(spec_path, 'r') as f:
        spec = json.load(f)

    # Extract domain info
    info = spec.get('info', {})
    title = info.get('title', 'Unknown')
    tags = spec.get('tags', [])
    tag_name = tags[0]['name'] if tags else 'Unknown'
    tag_slug = slugify(tag_name)
    domain = tag_slug.lower()

    # Extract all paths and their methods
    paths = spec.get('paths', {})
    url_paths = sorted(paths.keys())

    # Collect all operations: path -> method -> operationId
    operations = []
    for path in url_paths:
        methods = paths[path]
        for method in ['get', 'post', 'put', 'patch', 'delete']:
            if method in methods:
                op = methods[method]
                op_id = op.get('operationId', '')
                if op_id:
                    entity = extract_entity(path)
                    operations.append({
                        'path': path,
                        'method': method.upper(),
                        'operationId': op_id,
                        'entity': entity,
                    })

    # Build URL path strings for REGISTER_PLUGIN
    url_path_set = set()
    for op in operations:
        url_path_set.add(op['path'])
    url_path_list = sorted(url_path_set)

    # Class name: PascalCase the slug + 'Plugin'
    class_name = f'{pascal_case(tag_slug)}Plugin'

    # Derive .hpp path from the .cpp output path
    if output_path.endswith('.cpp'):
        hpp_path = output_path[:-4] + '.hpp'
    else:
        hpp_path = output_path + '.hpp'

    hpp_basename = os.path.basename(hpp_path)
    upper_prefix = tag_slug.upper()
    header_guard = f'{upper_prefix}_PLUGIN_HPP'

    paths_init = ', '.join(f'"{p}"' for p in url_path_list)

    # ------------------------------------------------------------------
    # Build the .hpp (inline class definition)
    # ------------------------------------------------------------------
    lines = []
    lines.append('/**')
    lines.append(f' * @file       {hpp_basename}')
    lines.append(f' * @brief      Auto-generated OOP plugin wrapper for {title}')
    lines.append(f' * @date       auto-generated')
    lines.append(f' *')
    lines.append(f' * DO NOT EDIT — regenerated from {os.path.basename(spec_path)}')
    lines.append(f' *')
    lines.append(f' * Handler stubs are protected virtual instance methods. Storage is')
    lines.append(f' * a protected instance member (m_storage). Initialize() delegates')
    lines.append(f' * handler registration to the overridable RegisterStubHandlers().')
    lines.append(f' */')
    lines.append(f'')
    lines.append(f'#ifndef {header_guard}')
    lines.append(f'#define {header_guard}')
    lines.append(f'')
    lines.append(f'#include "singleton/IPlugin.hpp"')
    lines.append(f'#include "singleton/PluginRegistration.hpp"')
    lines.append(f'#include "singleton/PluginManager.hpp"')
    lines.append(f'#include "storage/IStorageEngine.hpp"')
    lines.append(f'#include "storage/KeyBuilder.hpp"')
    lines.append(f'#include "nlohmann/json.hpp"')
    lines.append(f'')
    lines.append(f'#include <chrono>')
    lines.append(f'#include <cstdint>')
    lines.append(f'#include <iomanip>')
    lines.append(f'#include <random>')
    lines.append(f'#include <sstream>')
    lines.append(f'#include <string>')
    lines.append(f'')
    lines.append(f'using json = nlohmann::json;')
    lines.append(f'using namespace gnus::hash;')
    lines.append(f'')

    # Class declaration
    lines.append(f'///')
    lines.append(f'/// Auto-generated plugin class for {tag_name}.')
    lines.append(f'/// Derive from this class to override individual handlers without')
    lines.append(f'/// editing this generated file.')
    lines.append(f'///')
    lines.append(f'class {class_name} : public IPlugin')
    lines.append(f'{{')

    # Constants must be declared before REGISTER_PLUGIN expands — the macro
    # references kDefaultPluginPriority inline. Place them in a protected
    # block at the top of the class so the macro expansion sees them.
    lines.append(f'protected:')
    lines.append(f'    static constexpr unsigned int kDefaultPluginPriority  = 100;')
    lines.append(f'    static constexpr unsigned int kDefaultPaginationLimit = 50;')
    lines.append(f'    static constexpr unsigned int kUuidHexLength         = 32;')
    lines.append(f'    static constexpr uint8_t      kHexDigitMax           = 15;')
    lines.append(f'    static constexpr char         kPathSeparator         = \'/\';')
    lines.append(f'')
    lines.append(f'    IStorageEngine* m_storage = nullptr;')
    lines.append(f'')

    # REGISTER_PLUGIN references kDefaultPluginPriority (declared above) and
    # expands to public + private members. Place it after the constants.
    lines.append(f'    REGISTER_PLUGIN(kDefaultPluginPriority, ({{{paths_init}}}))')
    lines.append(f'')

    # Helper static methods
    lines.append(f'    /// Generate a random UUID (32 hex characters, no hyphens)')
    lines.append(f'    static std::string GenerateUuid()')
    lines.append(f'    {{')
    lines.append(f'        static thread_local std::mt19937 rng(std::random_device{{}}());')
    lines.append(f'        static thread_local std::uniform_int_distribution<uint8_t> dist(0, kHexDigitMax);')
    lines.append(f'        static constexpr const char* kHexChars = "0123456789abcdef";')
    lines.append(f'')
    lines.append(f'        std::string result;')
    lines.append(f'        result.reserve(kUuidHexLength);')
    lines.append(f'        for (unsigned int i = 0; i < kUuidHexLength; ++i)')
    lines.append(f'        {{')
    lines.append(f'            result += kHexChars[dist(rng)];')
    lines.append(f'        }}')
    lines.append(f'        return result;')
    lines.append(f'    }}')
    lines.append(f'')
    lines.append(f'    /// Get current UTC timestamp in ISO 8601 format')
    lines.append(f'    static std::string GetCurrentTimestamp()')
    lines.append(f'    {{')
    lines.append(f'        auto now = std::chrono::system_clock::now();')
    lines.append(f'        auto time = std::chrono::system_clock::to_time_t(now);')
    lines.append(f'        std::ostringstream oss;')
    lines.append(f'        oss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");')
    lines.append(f'        return oss.str();')
    lines.append(f'    }}')
    lines.append(f'')

    # Handler stub methods — protected virtual instance methods
    for op in operations:
        fn_body = generate_handler_method(
            op['operationId'], op['method'], op['path'], domain, op['entity'])
        lines.append(fn_body)
        lines.append(f'')

    # Overridable handler registration
    lines.append(_generate_register_stub_handlers(operations))
    lines.append(f'')

    # Public lifecycle
    lines.append(f'public:')
    lines.append(f'    ~{class_name}() override = default;')
    lines.append(f'')
    lines.append(f'    std::string GetName() override {{ return "{tag_slug}"; }}')
    lines.append(f'')
    lines.append(f'    bool Initialize(IServiceLocator& manager) noexcept override')
    lines.append(f'    {{')
    lines.append(f'        auto* pm = manager.GetService<PluginManager>(Fnv1a("PluginManager"));')
    lines.append(f'        if (pm == nullptr) {{ return false; }}')
    lines.append(f'        m_storage = manager.GetService<IStorageEngine>(Fnv1a("StorageEngine"));')
    lines.append(f'        if (m_storage == nullptr) {{ return false; }}')
    lines.append(f'        RegisterStubHandlers(pm);')
    lines.append(f'        return true;')
    lines.append(f'    }}')
    lines.append(f'')
    lines.append(f'    bool Shutdown() noexcept override')
    lines.append(f'    {{')
    lines.append(f'        return true;')
    lines.append(f'    }}')
    lines.append(f'')
    lines.append(f'    bool DeInit() noexcept override')
    lines.append(f'    {{')
    lines.append(f'        return true;')
    lines.append(f'    }}')
    lines.append(f'}};')
    lines.append(f'')
    lines.append(f'#endif // {header_guard}')
    lines.append(f'')

    # Write the .hpp
    hpp_dir = os.path.dirname(hpp_path)
    if hpp_dir:
        os.makedirs(hpp_dir, exist_ok=True)
    with open(hpp_path, 'w') as f:
        f.write('\n'.join(lines))

    print(f'Generated {hpp_path} ({len(operations)} handlers, {len(url_path_list)} paths)')

    # ------------------------------------------------------------------
    # Build the .cpp export shim (3 lines of content)
    # ------------------------------------------------------------------
    shim = (
        f'#include "{hpp_basename}"\n'
        f'EXPORT_PLUGIN({class_name})\n'
    )
    out_dir = os.path.dirname(output_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(output_path, 'w') as f:
        f.write(shim)

    print(f'Generated {output_path} (export shim)')

    # Write parent CMakeLists.txt if it doesn't exist (checked into repo).
    parent_dir = os.path.dirname(out_dir)
    parent_cmake = os.path.join(parent_dir, 'CMakeLists.txt') if parent_dir else ''
    if parent_cmake and parent_dir not in ('', '/', os.sep) and not os.path.exists(parent_cmake):
        cmake_content = (
            f'list(APPEND CMAKE_MODULE_PATH "${{CMAKE_CURRENT_SOURCE_DIR}}/../../cmake")\n'
            f'include(DomainPlugin)\n'
            f'build_domain_plugin({tag_slug} generated)\n'
        )
        with open(parent_cmake, 'w') as f:
            f.write(cmake_content)
        print(f'Created {parent_cmake} (checked into repo)')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f'Usage: {sys.argv[0]} <spec.json> <output.cpp>')
        sys.exit(1)
    generate(sys.argv[1], sys.argv[2])
