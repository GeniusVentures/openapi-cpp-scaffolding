# openapi-cpp-scaffolding (backend/scaffold)

Consumed as a submodule by GeniusNetwork parent repos — the parent pins this repo's develop branch and invokes its scripts from the parent's CMake. The global CLAUDE.md **Build-System Pre-PR Checklist** applies here; additionally:

- Plugin dylibs/DLLs must land flat in `${CMAKE_BINARY_DIR}/plugins` (`cmake/DomainPlugin.cmake`, `src/tunnel/CMakeLists.txt`, `src/archive/CMakeLists.txt`): RUNTIME + LIBRARY output properties plus per-config `<PROP>_<CONFIG>` entries.
- The parent invokes `scripts/fix_generated_destructors.py` with `--spec` — keep that CLI backward-compatible; a parent pinned to an older revision aborts its configure on argument mismatches.
- Python discovery uses `find_package(Python3 COMPONENTS Interpreter)` — never `find_program(python3)`, which fails on native Windows.
- Fresh non-recursive parent checkouts configure WITHOUT this submodule — the parent guards on `EXISTS backend/scaffold/CMakeLists.txt`, so nothing this repo requires may assume the parent's tree shape.
