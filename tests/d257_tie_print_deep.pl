#!/usr/bin/perl
use strict;
use warnings;
use feature 'say';
# D257: tied PRINT through a gensym, a hash copy, and STDOUT.
use Symbol qw(gensym);

package Cap257;
sub TIEHANDLE {
    my ($c, $ref) = @_;
    return bless \$ref, $c;
}
sub PRINT {
    my $it = shift;
    $$$it .= $_ for @_;
    return 1;
}
sub PRINTF {
    my $it = shift;
    my $fmt = shift;
    $$$it .= sprintf($fmt, @_);
    return 1;
}
package main;

my $s = "pre:";
my $new = gensym();
tie *$new, "Cap257", \$s;
print {$new} "ab";
say {$new} "c";
printf {$new} "%s", "Z";
my %h;
$h{fh} = $new;
print { $h{fh} } "Q";
print "s=[$s]\n";

package TieP257b;
sub TIEHANDLE { bless { s => "", marks => "" }, shift }
sub PRINT {
    my $o = shift;
    $o->{marks} .= "x";
    $o->{s} .= join("", @_);
    return 1;
}
sub UNTIE { $_[0]->{marks} .= "U"; }
package main;
my $cap = tie *STDOUT, "TieP257b";
print "A", "B";
say "C";
untie *STDOUT;
print "got=[$cap->{s}] marks=$cap->{marks}\n";
my $inner = "pre:";
my $mid = \$inner;
my $outer = \$mid;
$$$outer .= "Z";
print "bare=[$inner] istr=[$$$outer]\n";
