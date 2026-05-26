#!/usr/bin/env bash
# PreToolUse hook for Dusk Studio.
#
# Fires before Bash tool calls. If the command is a git commit / git push /
# gh pr create, examines the change set and reminds Claude to audit MANUAL.md
# whenever a user-visible source file (src/ui, src/session, src/dsp, src/engine)
# is changing. Skips silently if MANUAL.md is already part of the change set or
# if no user-visible files are touched.

set -e

input=$(cat)

tool=$(echo "$input" | jq -r '.tool_name // ""')
if [ "$tool" != "Bash" ]; then
  exit 0
fi

cmd=$(echo "$input" | jq -r '.tool_input.command // ""')

if ! echo "$cmd" | grep -qE '(^|[^a-zA-Z])(git[[:space:]]+commit|git[[:space:]]+push|gh[[:space:]]+pr[[:space:]]+create)'; then
  exit 0
fi

project_root=$(echo "$input" | jq -r '.cwd // ""')
if [ -z "$project_root" ] || [ ! -d "$project_root" ]; then
  project_root=$(git rev-parse --show-toplevel 2>/dev/null || true)
fi
if [ -n "$project_root" ] && [ -d "$project_root" ]; then
  cd "$project_root"
fi

manual_in_staged=$(git diff --cached --name-only 2>/dev/null | grep -c '^MANUAL\.md$' || true)
if [ "$manual_in_staged" -gt 0 ]; then
  exit 0
fi

mode=""
if echo "$cmd" | grep -qE '(^|[^a-zA-Z])git[[:space:]]+commit'; then
  mode="commit"
  changed=$(git diff --cached --name-only 2>/dev/null || true)
elif echo "$cmd" | grep -qE '(^|[^a-zA-Z])git[[:space:]]+push'; then
  mode="push"
  upstream=$(git rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || echo "")
  if [ -n "$upstream" ]; then
    changed=$(git diff --name-only "${upstream}...HEAD" 2>/dev/null || true)
  else
    changed=$(git diff --name-only origin/main...HEAD 2>/dev/null || git diff --name-only main...HEAD 2>/dev/null || true)
  fi
else
  mode="pr"
  changed=$(git diff --name-only origin/main...HEAD 2>/dev/null || git diff --name-only main...HEAD 2>/dev/null || true)
fi

if echo "$changed" | grep -q '^MANUAL\.md$'; then
  exit 0
fi

relevant=$(echo "$changed" | grep -E '^src/(ui|session|dsp|engine)/' || true)
if [ -z "$relevant" ]; then
  exit 0
fi

msg=$(printf 'MANUAL_SYNC_CHECK fired for %s.\n\nUser-visible source files in this change set:\n%s\n\nBefore the %s proceeds: audit MANUAL.md against these files. Update MANUAL.md (in the SAME commit if a commit; in a follow-up commit before the push/PR) when any of the following apply:\n  - new or removed UI control / panel / modal\n  - new or removed keyboard shortcut\n  - parameter range, default, or unit change\n  - new feature, new format support, new plugin host mode\n  - removed feature\n  - changed default behavior (e.g. autosave interval, take cap)\n  - changed file format or session schema (user-visible)\n\nSkip MANUAL.md when the change is purely internal: RT-safety, atomic ordering, lock-free refactor, build/test/CI, comment edits, dead-code removal, renames of private symbols. State explicitly in your response which side of the line this falls on. If unsure, audit by reading the changed files and comparing to MANUAL.md sections.' "$mode" "$relevant" "$mode")

jq -n --arg ctx "$msg" '{hookSpecificOutput: {hookEventName: "PreToolUse", additionalContext: $ctx}}'
