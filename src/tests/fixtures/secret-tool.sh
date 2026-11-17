#!/bin/sh
# Test runs must never read or write the desktop user's Secret Service.
printf '%s\n' 'Secret Service access is disabled for this test run.' >&2
exit 1
