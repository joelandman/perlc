#!/bin/bash
# D257: Pod::Text output_string writes through TiedOutFH PRINT.
# Not part of the byte harness (needs the host privlib on -I).
set -e
cd "$(dirname "$0")/.."
mkdir -p /tmp/pt
PRIV=$(perl -MConfig -e 'print $Config{privlib}')
cat > /tmp/pt/use_text_str.pl << 'EOF'
use strict;
use warnings;
use Pod::Text;
my $p = Pod::Text->new(sentence => 1, width => 78, errors => 'die');
my $out = "";
$p->output_string(\$out);
my $doc = "=head1 NAME\n\nWidget - a thing\n\n=cut\n";
$p->parse_string_document($doc);
print $out;
EOF
perl -I "$PRIV" /tmp/pt/use_text_str.pl > /tmp/pt/ps.out 2>/tmp/pt/ps.err
./perlc -I "$PRIV" /tmp/pt/use_text_str.pl -o /tmp/pt/use_text_str.bin
/tmp/pt/use_text_str.bin > /tmp/pt/bs.out 2>/tmp/pt/bs.err
diff -u /tmp/pt/ps.out /tmp/pt/bs.out
diff -u /tmp/pt/ps.err /tmp/pt/bs.err
echo D257_OK
