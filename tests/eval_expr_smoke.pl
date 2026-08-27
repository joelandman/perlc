#!/usr/bin/perl
# Smoke: string eval EXPR (constant and dynamic).
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

check('const_add', eval("1+2") == 3);
check('const_q', eval(q{ 10 * 4 }) == 40);
my $code = "2+3";
check('dyn_add', eval($code) == 5);
check('syntax_sets_at', do { eval "this is { not perl"; $@ ne "" });
check('die_caught', do { eval "die 'boom'"; $@ =~ /boom/ });
{
    my $x = 21;
    check('const_sees_my', eval('$x + 1') == 22);
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "eval_expr_smoke_done\n";
