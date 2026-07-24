#!/usr/bin/perl
# Deep: D95 — list vs string repetition for x.
use strict;
# no warnings: real Perl emits "Useless use of a constant in void context"
# on scalar `(1,2,3)` comma-operator; perlc has no warnings system (D56).

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub f { "x" }
sub g { wantarray() ? (1, 2) : 9 }

{
    my @a = (f() x 2);
    check('bare_call', scalar(@a) == 1 && $a[0] eq "xx");
}
{
    my @a = ((f()) x 2);
    check('paren_call', scalar(@a) == 2 && join(",", @a) eq "x,x");
}
{
    my @a = ((1, 2) x 2);
    check('list_lit', scalar(@a) == 4 && join(",", @a) eq "1,2,1,2");
}
{
    my @a = (qw(a b) x 2);
    check('qw_list', scalar(@a) == 4 && join(",", @a) eq "a,b,a,b");
}
{
    my @a = ("ab" x 2);
    check('str_lit', scalar(@a) == 1 && $a[0] eq "abab");
}
{
    my $s = (f() x 2);
    check('scalar_str_rep', $s eq "xx");
}
{
    my @a = ((g()) x 2);
    check('paren_listret', scalar(@a) == 4 && join(",", @a) eq "1,2,1,2");
}
{
    my $a = (10);
    check('scalar_paren_one', $a == 10);
}
{
    my $b = (1, 2, 3);
    check('scalar_paren_last', $b == 3);
}
{
    my ($c) = (10);
    check('list_assign_paren', $c == 10);
}
{
    my @d = (f() x 0);
    check('x_zero_str', scalar(@d) == 1 && $d[0] eq "");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d95_repeat_list_vs_str_done\n";
