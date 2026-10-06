#!/bin/sh
# The oracle harness. SQLite builds every database and answers every query;
# sqlr answers the same queries; the bytes must match exactly.
#
# The sqlite3 shell is not present in this environment, so SQLite is reached
# through python3's stdlib binding to the same library. See README, "The oracle".
set -e
cd "$(dirname "$0")/.."
[ -x ./sqlr ] || make sqlr
exec python3 t/oracle.py "$@"
