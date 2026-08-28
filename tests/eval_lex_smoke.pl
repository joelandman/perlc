#!/usr/bin/perl
# Smoke: string eval sees outer my (dynamic strings + eval-defined subs).
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

{
    my $x = 21;
    my $code = '$x + 1';
    check('dyn_sees_my', eval($code) == 22);
}

{
    my $x = 7;
    eval '$x = 99';
    check('dyn_assign', $x == 99);
}

{
    my @a = (1, 2, 3);
    my $code = 'push @a, 4; join(",", @a)';
    check('dyn_array', eval($code) eq '1,2,3,4');
}

{
    my $n = 10;
    eval 'sub eval_lex_f { $n }';
    check('sub_sees', eval_lex_f() == 10);
    $n = 20;
    check('sub_mut', eval_lex_f() == 20);
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "eval_lex_smoke_done\n";
