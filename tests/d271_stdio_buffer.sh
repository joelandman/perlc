#!/bin/bash
# D271: perl resets STDOUT/STDERR buffering after a parent stdbuf.
# The harness does not wrap itself in stdbuf, so the .pl pair cannot
# see this. make test-all runs this script.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=/tmp/pt/d271
rm -rf "$ROOT"
mkdir -p "$ROOT"
cat > "$ROOT/ord.pl" << 'EOF'
print "line\n";
print STDERR "err\n";
print "partial";
warn "mid\n";
print "tail\n";
EOF
./perlc "$ROOT/ord.pl" -o "$ROOT/ord" >"$ROOT/cerr" 2>&1
stdbuf -oL -eL perl "$ROOT/ord.pl" >"$ROOT/perl.txt" 2>&1 || true
stdbuf -oL -eL "$ROOT/ord" >"$ROOT/perlc.txt" 2>&1 || true
if ! diff -u "$ROOT/perl.txt" "$ROOT/perlc.txt"; then
    echo "D271 stdbuf -oL order mismatch" >&2
    exit 1
fi
stdbuf -o0 -e0 perl "$ROOT/ord.pl" >"$ROOT/perl0.txt" 2>&1 || true
stdbuf -o0 -e0 "$ROOT/ord" >"$ROOT/perlc0.txt" 2>&1 || true
if ! diff -u "$ROOT/perl0.txt" "$ROOT/perlc0.txt"; then
    echo "D271 stdbuf -o0 order mismatch" >&2
    exit 1
fi
# A tty is line-buffered: the newline flushes before the warn.
python3 - "$ROOT/ord.pl" "$ROOT/ord" << 'PY'
import os, pty, select, subprocess, sys
def run(cmd):
    master, slave = pty.openpty()
    p = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)
    data = b""
    while True:
        r, _, _ = select.select([master], [], [], 2)
        if not r:
            break
        try:
            chunk = os.read(master, 4096)
        except OSError:
            break
        if not chunk:
            break
        data += chunk
    p.wait()
    os.close(master)
    return data.replace(b"\r\n", b"\n")
pl, pc = sys.argv[1], sys.argv[2]
a = run(["stdbuf", "-o0", "-e0", "perl", pl])
b = run(["stdbuf", "-o0", "-e0", pc])
if a != b:
    sys.stderr.write("D271 tty order mismatch\nperl=%r\nperlc=%r\n" % (a, b))
    sys.exit(1)
PY
echo D271_OK
