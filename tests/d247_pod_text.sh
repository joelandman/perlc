#!/bin/bash
# D247: Pod::Text formats a small document the same way perl does.
# Not part of the byte harness (needs the host privlib on -I).
set -e
cd "$(dirname "$0")/.."
mkdir -p /tmp/pt
PRIV=$(perl -MConfig -e 'print $Config{privlib}')
cat > /tmp/pt/use_text.pl << 'EOF'
use strict;
use warnings;
print STDERR "before use\n";
use Pod::Text;
print STDERR "after ver=$Pod::Text::VERSION\n";
my $p = Pod::Text->new(sentence => 1, width => 78);
print STDERR "ref=", ref($p), "\n";
my $doc = "=head1 NAME\n\nWidget - a thing\n\n=cut\n";
$p->parse_string_document($doc);
EOF
perl -I "$PRIV" /tmp/pt/use_text.pl > /tmp/pt/p.out 2>/tmp/pt/p.err
./perlc -I "$PRIV" /tmp/pt/use_text.pl -o /tmp/pt/use_text.bin
/tmp/pt/use_text.bin > /tmp/pt/b.out 2>/tmp/pt/b.err
diff -u /tmp/pt/p.out /tmp/pt/b.out
diff -u /tmp/pt/p.err /tmp/pt/b.err
echo D247_OK
