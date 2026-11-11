#!/bin/bash
# D276: ./perlc -g used to fail verify because main's DISubprogram was
# also attached to __perlc_init_globals. make test-all runs this script.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=/tmp/pt/d276
rm -rf "$ROOT"
mkdir -p "$ROOT"
printf '%s\n' 'print "dbg\n";' > "$ROOT/t.pl"
./perlc -g "$ROOT/t.pl" -o "$ROOT/t" >"$ROOT/cerr" 2>&1
if grep -q 'DISubprogram' "$ROOT/cerr"; then
    echo "D276 verify still fails" >&2
    cat "$ROOT/cerr" >&2
    exit 1
fi
got=$("$ROOT/t")
if [ "$got" != "dbg" ]; then
    echo "D276 run mismatch: [$got]" >&2
    exit 1
fi
echo D276_OK
