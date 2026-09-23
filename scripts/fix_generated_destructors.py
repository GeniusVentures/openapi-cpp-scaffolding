#!/usr/bin/env python3
"""
Post-process generated C++ model files.

Two fixes the cpp-pistache/cpp-restbed templates need:

1. Destructor syntax:
       ClassName::~ClassName = default;   →   ClassName::~ClassName() = default;

2. Null guards for optional fields in from_json. The generator emits
       if(j.find("field") != j.end()) { j.at("field").get_to(...); ... }
   which throws nlohmann::json::type_error when the payload carries the
   documented `"field": null` representation of an absent optional (e.g.
   OrderCreate.table_id for tableless quick-orders, Location.tax_rate
   unconfigured). Guarding with !j.at("field").is_null() leaves the field
   unset (FieldIsSet() stays false) instead of throwing.
"""

import re
import sys


def fix_destructor_syntax(content: str) -> str:
    """Fix ~ClassName = default; → ~ClassName() = default;"""
    return re.sub(
        r'(~\w+)\s*=\s*default;',
        r'\1() = default;',
        content
    )


def fix_null_guards(content: str) -> str:
    """Guard optional-field from_json blocks against explicit nulls.

    Rewrites the generator-emitted
        if(j.find("x") != j.end())
    to
        if(j.find("x") != j.end() && !j.at("x").is_null())
    The pattern only appears in from_json optional-field blocks (required
    fields use bare j.at(...).get_to(...); to_json uses IsSet checks), so a
    uniform rewrite is safe.
    """
    return re.sub(
        r'if\(j\.find\(("[^"]+")\) != j\.end\(\)\)',
        r'if(j.find(\1) != j.end() && !j.at(\1).is_null())',
        content
    )


def fix_file(filepath: str) -> bool:
    """Apply all generated-code fixes to one file. True when it changed."""
    with open(filepath, 'r') as f:
        content = f.read()

    fixed = fix_destructor_syntax(fix_null_guards(content))

    if fixed != content:
        with open(filepath, 'w') as f:
            f.write(fixed)
        return True
    return False


if __name__ == '__main__':
    count = 0
    for path in sys.argv[1:]:
        if fix_file(path):
            count += 1
    print(f'Fixed generated code in {count} file(s)')
