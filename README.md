# OpenAPI C++ Scaffolding

C++ backend scaffold for GeniusNetwork projects. Provides a plugin framework, storage engine, OpenAPI code generation, and a Boost.Beast HTTP server with Swagger UI.

## Quick Start

```bash
# Clone as a submodule into your project
git submodule add git@github.com:GeniusVentures/openapi-cpp-scaffolding.git backend/scaffold

# Copy the parent integration template
cp backend/scaffold/CMakeLists.txt.example CMakeLists.txt

# Add your OpenAPI specs
mkdir -p api-specs
cp your_domain_openapi.json api-specs/

# Build
cd build/OSX/Debug
cmake ../.. -G "Ninja" -DCMAKE_BUILD_TYPE=Debug
ninja
```

## CMakeLists.txt.example

`CMakeLists.txt.example` at the repository root shows how a parent project wires up this scaffold. It documents:

- **`add_subdirectory(scaffold)`** — how to include the scaffold in your build
- **`api-specs/`** — where to place project-specific OpenAPI specs, and how cmake copies + installs them next to the server binary
- **`backend/`** — where handwritten implementations (config loaders, storage managers, plugin overrides) live alongside the scaffold
- **`frontend/scaffold/`** — integration point for the `openapi-client-scaffold` submodule

Copy it to your project root as a starting point and adapt the project name and paths.

## Directory Layout

```
backend/scaffold/
├── CMakeLists.txt              # Scaffold root — add_subdirectory(src) + add_subdirectory(json)
├── CMakeLists.txt.example      # Parent project integration template
├── cmake/
│   └── DomainPlugin.cmake      # build_domain_plugin() macro
├── json/
│   ├── CMakeLists.txt          # Copies + installs specs next to binary
│   └── identity_openapi.json   # Built-in identity domain spec
├── scripts/
│   ├── generate_plugin.py      # Generates plugin wrapper from OpenAPI spec
│   └── fix_generated_destructors.py
├── src/
│   ├── base/                   # Server binary (genius_ai_server) + Swagger UI
│   ├── singleton/              # PluginManager, CServiceLocator, FNV-1a hashing
│   ├── storage/                # IStorageEngine, RocksDBEngine, KeyBuilder
│   ├── outcome/                # outcome::result error handling
│   ├── identity/               # Auth plugin (JWT, PBKDF2, login/logout/refresh)
│   └── tunnel/                 # Cloudflare Tunnel subprocess manager
└── tests/                      # Scaffold test suite
```
