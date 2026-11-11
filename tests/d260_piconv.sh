#!/bin/bash
# Curated piconv invocation from tests/realworld/run_compare.sh:
# piconv -f ascii -t ascii, stdin abc. Byte-for-byte vs perl.
set -u
cd "$(dirname "$0")/.."
PRIV=/home/joe/local/lib/perl5/5.44.0
rm -f /tmp/pt/piconv260
./perlc /usr/bin/piconv -o /tmp/pt/piconv260 >/tmp/pt/piconv260.compile 2>&1 || {
    echo COMPILE_FAIL
    cat /tmp/pt/piconv260.compile
    exit 1
}
printf '%s' 'abc' | perl -I "$PRIV" /usr/bin/piconv -f ascii -t ascii >/tmp/pt/piconv260.perl 2>/tmp/pt/piconv260.perl.err
printf '%s' 'abc' | /tmp/pt/piconv260 -f ascii -t ascii >/tmp/pt/piconv260.perlc 2>/tmp/pt/piconv260.perlc.err
if cmp -s /tmp/pt/piconv260.perl /tmp/pt/piconv260.perlc && cmp -s /tmp/pt/piconv260.perl.err /tmp/pt/piconv260.perlc.err; then
    echo D260_OK
    exit 0
fi
echo D260_DIFF
echo '--- perl stdout ---'; cat /tmp/pt/piconv260.perl; echo
echo '--- perlc stdout ---'; cat /tmp/pt/piconv260.perlc; echo
echo '--- perl stderr ---'; cat /tmp/pt/piconv260.perl.err
echo '--- perlc stderr ---'; cat /tmp/pt/piconv260.perlc.err
exit 1
