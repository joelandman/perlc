#!/usr/bin/perl
# D220: TIESCALAR / TIEHANDLE return values, bareword and expression class.
use strict;
use warnings;
no warnings 'once';
package TieSD;
sub TIESCALAR {
    my ($class, $v, $w) = @_;
    return bless { v => $v, w => $w }, $class;
}
package TieHD;
sub TIEHANDLE {
    my ($class, $s) = @_;
    return bless { s => (defined $s ? $s : "none"), n => 1 }, $class;
}
package main;
my $x;
my $o = tie $x, "TieSD", 3, 4;
print "sref=", ref($o), "\n";
print "sv=", $o->{v}, ",", $o->{w}, "\n";

my $name = "TieHD";
my $arg = "zz";
my $out = \*OUT;
my $h = tie *$out, $name, $arg;
print "href=", ref($h), "\n";
print "hs=", $h->{s}, " n=", $h->{n}, "\n";

my $g = \*Dyn;
my $h2 = tie *$g, $name, "yy";
print "h2=", ref($h2), " ", $h2->{s}, "\n";
print "done\n";
