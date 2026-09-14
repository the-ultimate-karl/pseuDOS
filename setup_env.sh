#!/usr/bin/env bash
# ==============================================================================
# pseuDOS Host Environment Setup (Bash / Shell)
# Convenience wrapper for check_env.sh
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${SCRIPT_DIR}/check_env.sh" "$@"
