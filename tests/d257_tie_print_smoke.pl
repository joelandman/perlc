#!/usr/bin/perl
use strict;
use warnings;
use feature 'say';
# D257: print/say/printf on a tied handle call PRINT/PRINTF once.
package TieP257;
sub TIEHANDLE {
    my ($c, $tag) = @_;
    return bless { tag => $tag, marks => "", log => "" }, $c;
}
sub PRINT {
    my $o = shift;
    $o->{marks} .= "x";
    $o->{log} .= "P:" . join("|", @_) . ";";
    return 1;
}
sub PRINTF {
    my $o = shift;
    $o->{marks} .= "x";
    $o->{log} .= "F:" . join(",", @_) . ";";
    return 7;
}
package main;
my $o = tie *OUT257, "TieP257", "t";
print OUT257 "a", "b";
say OUT257 "c";
{
    local $, = ",";
    local $\ = "Z";
    print OUT257 "d", "e";
}
sub take257 { return printf OUT257 "%s-%d", "x", 2; }
my $r = take257();
print "marks=$o->{marks} r=$r log=$o->{log}\n";
print "tag=$o->{tag}\n";
