# Sent inline by windows-source.sh before the preflight: the guest's clock in
# UTC, which the host compares with its own.
"regress:utc {0}" -f [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
exit 0
