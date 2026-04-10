#\!/usr/bin/env python3
"""Generate compile_commands.json from make dry-run output."""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

def parse_compile_command(line, cwd):
    """Parse a compilation command line and extract file and command."""
    # Match gcc/cc/g++/clang commands
    if not re.search(r'\b(gcc|g\+\+|cc|c\+\+|clang|clang\+\+)\b', line):
        return None
    
    # Find source file (.c, .cc, .cpp, .cxx)
    source_match = re.search(r'\S+\.(c|cc|cpp|cxx|C)(?:\s|$)', line)
    if not source_match:
        return None
    
    source_file = source_match.group(0).strip()
    
    # Make path absolute
    if not os.path.isabs(source_file):
        source_file = os.path.join(cwd, source_file)
    source_file = os.path.normpath(source_file)
    
    return {
        'directory': cwd,
        'command': line.strip(),
        'file': source_file
    }

def generate_from_make(srcdir='.'):
    """Generate compile_commands.json by parsing make dry-run."""
    cwd = os.path.abspath(srcdir)
    os.chdir(cwd)
    
    print("Running make dry-run to extract compile commands...", file=sys.stderr)
    
    try:
        # Run make -n to get commands without executing
        result = subprocess.run(
            ['make', '-n'],
            capture_output=True,
            text=True,
            cwd=cwd
        )
        
        commands = []
        seen_files = set()
        
        for line in result.stdout.splitlines():
            entry = parse_compile_command(line, cwd)
            if entry and entry['file'] not in seen_files:
                commands.append(entry)
                seen_files.add(entry['file'])
        
        if not commands:
            print("Warning: No compilation commands found", file=sys.stderr)
            return []
        
        return commands
        
    except subprocess.CalledProcessError as e:
        print(f"Error running make: {e}", file=sys.stderr)
        return []

def main():
    srcdir = sys.argv[1] if len(sys.argv) > 1 else '.'
    output = 'compile_commands.json'
    
    commands = generate_from_make(srcdir)
    
    if commands:
        with open(output, 'w') as f:
            json.dump(commands, f, indent=2)
        print(f"Generated {output} with {len(commands)} entries", file=sys.stderr)
    else:
        print("Failed to generate compile_commands.json", file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    main()
