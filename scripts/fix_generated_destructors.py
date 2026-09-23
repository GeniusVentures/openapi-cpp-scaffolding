#!/usr/bin/env python3
"""
Post-process generated C++ model files.

Two fixes the cpp-pistache/cpp-restbed templates need:

1. Destructor syntax:
       ClassName::~ClassName = default;   →   ClassName::~ClassName() = default;

2. Null guards for NULLABLE optional fields in from_json. The generator emits
       if(j.find("field") != j.end()) { j.at("field").get_to(...); ... }
   which throws nlohmann::json::type_error when the payload carries an
   explicit null. For schema-nullable properties (e.g. OrderCreate.table_id
   for tableless quick-orders, Location.tax_rate unconfigured) the documented
   `"field": null` representation must leave the field unset, so those blocks
   gain `&& !j.at("field").is_null()`. For optional fields that are NOT
   nullable (e.g. UserUpdate.email) an explicit null is a client error — the
   throw is the correct 400 path — so they keep the bare form. Nullability is
   read from the spec (--spec); without a spec no null guards are touched.

   The rewrite is convergent: bare → guarded for nullable properties, and a
   previously blanket-applied guard → bare for non-nullable ones.

Usage:
    fix_generated_destructors.py [--spec <openapi.json>] <file.cpp|dir>...
"""

import json
import re
import sys
from pathlib import Path

_BARE_GUARD = re.compile(r'if\(j\.find\(("[^"]+")\) != j\.end\(\)\)')
_GUARDED_FORM = r'if(j.find(\1) != j.end() && !j.at(\1).is_null())'
_GUARDED_GUARD = re.compile(
    r'if\(j\.find\(("[^"]+")\) != j\.end\(\) && !j\.at\(\1\)\.is_null\(\)\)')


def _property_is_nullable(prop):
    """True for OpenAPI 3.0 nullable:true or 3.1 type/anyOf null markers."""
    if prop.get("nullable") is True:
        return True
    ptype = prop.get("type")
    if isinstance(ptype, list) and "null" in ptype:
        return True
    return any(item == {"type": "null"} for item in prop.get("anyOf", []))


def load_nullable_properties(spec_path):
    """Map lowercase model name → set of nullable property names.

    Every schema gets an entry (empty when nothing is nullable) so a model
    with no nullable properties is distinguishable from no spec at all.
    """
    with open(spec_path, 'r') as f:
        spec = json.load(f)
    schemas = spec.get("components", {}).get("schemas", {})
    nullable = {}
    for name, schema in schemas.items():
        props = set()
        for prop_name, prop in schema.get("properties", {}).items():
            if _property_is_nullable(prop):
                props.add(prop_name)
        nullable[name.lower()] = props
    return nullable


def fix_destructor_syntax(content):
    """Fix ~ClassName = default; → ~ClassName() = default;"""
    return re.sub(
        r'(~\w+)\s*=\s*default;',
        r'\1() = default;',
        content
    )


def fix_null_guards(content, nullable_props):
    """Converge optional-field guards toward the spec's nullability.

    BARE → guarded for nullable properties; guarded → BARE for everything
    else (undoing a previously blanket-applied guard). None means no spec
    was supplied — leave null guards untouched. The if(j.find(...)) pattern
    only appears in from_json optional-field blocks (required fields use
    bare j.at(...).get_to(...); to_json uses IsSet checks), so a uniform
    rewrite is safe.
    """
    if nullable_props is None:
        return content

    def guard(match):
        prop = match.group(1)[1:-1]
        if prop in nullable_props:
            return match.expand(_GUARDED_FORM)
        return match.group(0)

    content = _GUARDED_GUARD.sub(
        lambda m: m.group(0) if m.group(1)[1:-1] in nullable_props
        else 'if(j.find({}) != j.end())'.format(m.group(1)),
        content
    )
    return _BARE_GUARD.sub(guard, content)


def fix_file(filepath, nullable_by_model):
    """Apply all generated-code fixes to one file. True when it changed."""
    with open(filepath, 'r') as f:
        content = f.read()

    model = Path(filepath).stem.lower()
    fixed = fix_destructor_syntax(
        fix_null_guards(content, nullable_by_model.get(model)))

    if fixed != content:
        with open(filepath, 'w') as f:
            f.write(fixed)
        return True
    return False


def main():
    args = sys.argv[1:]
    nullable_by_model = {}
    paths = []
    spec_path = None
    it = iter(args)
    for arg in it:
        if arg == '--spec':
            spec_path = next(it, None)
            if not spec_path:
                sys.exit('fix_generated_destructors: --spec requires a path')
        else:
            paths.append(arg)

    if spec_path:
        nullable_by_model = load_nullable_properties(spec_path)

    targets = []
    for path in paths:
        target = Path(path)
        if target.is_dir():
            targets.extend(sorted(target.glob('*.cpp')))
        else:
            targets.append(target)

    count = 0
    for target in targets:
        if fix_file(str(target), nullable_by_model):
            count += 1
    print('Fixed generated code in {} file(s)'.format(count))


if __name__ == '__main__':
    main()
