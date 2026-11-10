#!/bin/bash
# D258: formatting codes and verbatim indent survive Pod::Text's print path.
# Not part of the byte harness (needs the host privlib on -I).
set -e
cd "$(dirname "$0")/.."
mkdir -p /tmp/pt
PRIV=$(perl -MConfig -e 'print $Config{privlib}')
cat > /tmp/pt/use_text_fmt.pl << 'EOF'
use strict;
use warnings;
use Pod::Text;
my $p = Pod::Text->new(sentence => 1, width => 40, errors => 'die', quotes => '"');
my $out = "";
$p->output_string(\$out);
my $doc = << 'POD';
=head1 NAME

Widget - a thing

=head1 DESCRIPTION

This is a sentence. This is another.

See L<Widget|Widget> and C<code> and B<bold> and I<ital>.

=over 4

=item one

The first item.

=item two

The second.

=back

A verbatim block:

  line one
  line two

=cut
POD
$p->parse_string_document($doc);
print $out;
EOF
perl -I "$PRIV" /tmp/pt/use_text_fmt.pl > /tmp/pt/pf.out 2>/tmp/pt/pf.err
./perlc -I "$PRIV" /tmp/pt/use_text_fmt.pl -o /tmp/pt/use_text_fmt.bin
timeout 25 /tmp/pt/use_text_fmt.bin > /tmp/pt/bf.out 2>/tmp/pt/bf.err
diff -u /tmp/pt/pf.out /tmp/pt/bf.out
diff -u /tmp/pt/pf.err /tmp/pt/bf.err
echo D258_OK
