#!/usr/bin/perl
# Deep: glob re-alias, *a = *b copy, ref-in-scalar, lexical shadow.
use strict;
use warnings;
no strict 'vars';
no warnings 'once';

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

{
    my $x = "a";
    my $y = "b";
    *g = \$x;
    check('first', $g eq "a");
    *g = \$y;
    check('rebind', $g eq "b");
    $g = "c";
    check('rebind_write', $y eq "c" && $x eq "a");
}

{
    my @src = (10, 20);
    my $r = \@src;
    *via = $r;
    check('via_ref', $via[1] == 20);
    $via[1] = 21;
    check('via_write', $src[1] == 21);
}

{
    my %h = (z => 1);
    *one = \%h;
    *two = *one;
    check('copy_read', $two{z} == 1);
    $two{z} = 2;
    check('copy_write', $h{z} == 2);
}

{
    my $lex = "L";
    *lex = \$lex;   # package $lex vs lexical
    {
        my $lex = "inner";
        check('shadow', $lex eq "inner");
    }
    check('after_shadow', $lex eq "L");
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "glob_slot_deep_done\n";
