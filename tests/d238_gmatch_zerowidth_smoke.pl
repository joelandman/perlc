#!/usr/bin/perl
# D238: /g on a pattern that can match empty must finish.
# Pod::Simple::parse_string_document splits on this shape.
use strict;
use warnings;

my $doc = "=head1 NAME\n\nWidget - a thing\n\n=cut\n";
pos($doc) = 0;
my $n = 0;
while ($doc =~ m/([^\n\r]*)(\r?\n?)/g) {
    my ($g1, $g2) = ($1, $2);
    my $p = defined(pos($doc)) ? pos($doc) : "U";
    my $take = (length($g1 // "") || length($g2 // "")
                || (defined(pos($doc)) && pos($doc) != length($doc))) ? "Y" : "N";
    $g1 = "" unless defined $g1;
    $g2 = "" unless defined $g2;
    $g2 =~ s/\n/\\n/g;
    $g2 =~ s/\r/\\r/g;
    print "take=$take 1=[$g1] 2=[$g2] pos=$p\n";
    last if ++$n > 20;
}
my $end = defined(pos($doc)) ? pos($doc) : "U";
print "done n=$n pos=$end\n";
