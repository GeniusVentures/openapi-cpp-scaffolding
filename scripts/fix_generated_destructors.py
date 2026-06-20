#!/usr/bin/env python3
"""
Post-process generated C++ files to fix destructor syntax.
OpenAPI Generator's cpp-restbed-server template generates:
    ClassName::~ClassName = default;
But Clang requires:
    ClassName::~ClassName() = default;
"""

import re
import sys


def fix_destructor_syntax(filepath: str) -> bool:
    """Fix ~ClassName = default; → ~ClassName() = default;"""
    with open(filepath, 'r') as f:
        content = f.read()

    # Pattern: ClassName::~ClassName = default;
    # Replace with: ClassName::~ClassName() = default;
    fixed = re.sub(
        r'(~\w+)\s*=\s*default;',
        r'\1() = default;',
        content
    )

    if fixed != content:
        with open(filepath, 'w') as f:
            f.write(fixed)
        return True
    return False


if __name__ == '__main__':
    count = 0
    for path in sys.argv[1:]:
        if fix_destructor_syntax(path):
            count += 1
    print(f'Fixed destructor syntax in {count} file(s)')
