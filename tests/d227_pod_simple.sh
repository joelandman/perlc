#!/bin/bash
# D227: Pod::Simple's real use-chain compiles and a small constructor
# matches perl. Not part of the byte harness (needs the host privlib).
set -e
cd "$(dirname "$0")/.."
mkdir -p /tmp/pt
PRIV=$(perl -MConfig -e 'print $Config{privlib}')
cat > /tmp/pt/use_simple.pl << 'EOF'
use strict;
use warnings;
print STDERR "before use\n";
use Pod::Simple;
print STDERR "after use ver=$Pod::Simple::VERSION\n";
my $p = Pod::Simple->new;
print STDERR "ref=", ref($p), "\n";
print STDERR "tabs=", $p->expand_verbatim_tabs, "\n";
print STDERR "pv=", $p->VERSION, "\n";
EOF
perl -I "$PRIV" /tmp/pt/use_simple.pl > /tmp/pt/p.out 2>/tmp/pt/p.err
./perlc -I "$PRIV" /tmp/pt/use_simple.pl -o /tmp/pt/use_simple.bin
/tmp/pt/use_simple.bin > /tmp/pt/b.out 2>/tmp/pt/b.err
diff -u /tmp/pt/p.out /tmp/pt/b.out
diff -u /tmp/pt/p.err /tmp/pt/b.err
echo D227_OK
