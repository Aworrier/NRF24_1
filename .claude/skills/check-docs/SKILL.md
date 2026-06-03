---
name: check-docs
description: Use when source files in main/ are modified — checks docs/ folder for stale references (file paths, function names, commands, config values) against the current codebase. Also invoke manually via /check-docs before committing.
---

# Check Docs for Staleness

After modifying C source files, verify that `docs/` still accurately describes the code.

## Check Dimensions

Run these checks in order. Report each finding with file:line and suggested fix.

### 1. File Path References

Grep docs for file paths (e.g., `../main/`, `./`, `tools/`). For each path found in docs, verify the target file exists.

```bash
grep -roPn '(?:\\.\\.?/|[a-z_]+/)[a-z_/]+\\.(?:c|h|py|sh|md)' docs/ | sort -u
```

### 2. Function Name References

Extract function names mentioned in docs, then grep source files to confirm each exists.

```bash
# Extract function names from docs (backtick-wrapped identifiers suggesting functions)
grep -roPn '`[a-z][a-z_0-9]+\(' docs/ | sed 's/.*`//; s/($//' | sort -u

# For each, verify in main/
grep -rn "FUNCNAME" main/
```

### 3. Commands / Scripts

Grep docs for shell commands (`idf.py`, `bash`, `./flash.sh`, etc.). Verify each is still valid:

- `./flash.sh` → actual path is `tools/flash.sh` (or use `/flash` skill)
- `idf.py <cmd>` → check CLI_REFERENCE.md or run `idf.py --help`
- `bash tools/flash.sh` → verify file exists

### 4. Config / Kconfig Values

Grep docs for `CONFIG_NRF24_*` symbols. Cross-reference with `sdkconfig` and `main/Kconfig.projbuild`:

```bash
grep -roPn 'CONFIG_NRF24_[A-Z_]+' docs/ | sort -u
```

For each symbol found in docs:
- Check it exists in `main/Kconfig.projbuild`
- If a default value is stated, verify against `sdkconfig`

### 5. Build / Flash Instructions

Build/flash instructions in docs should reference `tools/flash.sh` or `idf.py` commands, not non-existent root-level scripts. Cross-check with CLAUDE.md which is authoritative for build commands.

## Output Format

Report findings grouped by severity:

- **BROKEN**: Path doesn't exist, function not found, command would fail
- **MISLEADING**: Technically correct but likely to confuse (e.g., describes only part of the system)
- **STALE**: Was correct, now outdated by recent changes

## Hook Integration

A `PostToolUse` hook monitors edits to `main/*.{c,h}`. After source modifications, invoke `/check-docs` before committing. The hook writes a marker to `.claude/.docs_stale_marker` — check this file's timestamp to know when docs need review.
