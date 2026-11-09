#!/bin/bash
# D251: require EXPR searches directories passed to perlc with -I.
# The harness does not pass -I, so this stays a shell compare.
set -e
cd "$(dirname "$0")/.."
mkdir -p /tmp/pt/inc251
echo '1;' > /tmp/pt/inc251/D251Only.pm
cat > /tmp/pt/req251.pl << 'EOF'
use strict;
use warnings;
my $r = eval { require D251Only };
print "r=", defined($r) ? $r : "U", "\n";
EOF
perl -I /tmp/pt/inc251 /tmp/pt/req251.pl > /tmp/pt/p.out 2>/tmp/pt/p.err
./perlc -I /tmp/pt/inc251 /tmp/pt/req251.pl -o /tmp/pt/req251.bin
/tmp/pt/req251.bin > /tmp/pt/b.out 2>/tmp/pt/b.err
diff -u /tmp/pt/p.out /tmp/pt/b.out
diff -u /tmp/pt/p.err /tmp/pt/b.err
echo D251_OK
