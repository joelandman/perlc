#!/usr/bin/perl
# D220: tie with an expression class, and tie of a glob.
use strict;
use warnings;
no warnings 'once';
package TieS;
sub TIESCALAR {
    my ($class, $v) = @_;
    return bless { v => $v }, $class;
}
package main;
my $x = "old";
my $o = tie $x, 'TieS', 9;
print "ref=", ref($o), "\n";
print "v=", $o->{v}, "\n";

package TieH;
sub TIEHANDLE {
    my ($class, $s) = @_;
    return bless { s => $s }, $class;
}
package main;
my $g = \*G;
my $cls = "TieH";
my $s = "payload";
my $h = tie *$g, $cls, $s;
print "href=", ref($h), "\n";
print "hs=", $h->{s}, "\n";
