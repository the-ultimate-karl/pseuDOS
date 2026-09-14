# ==============================================================================
# pseuDOS Host Environment Setup (PowerShell)
# Convenience wrapper for check_env.ps1
# ==============================================================================

$PSScript = Join-Path $PSScriptRoot "check_env.ps1"
& $PSScript @args
exit $LASTEXITCODE
