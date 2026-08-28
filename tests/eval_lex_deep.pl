#!/usr/bin/perl
# Deep: string eval outer lexicals — scalars, arrays, hashes, shadowing, closures.
use strict;
use warnings;
no warnings 'void';

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

{
    my $a = 3;
    my $b = 4;
    my $src = '$a * $b';
    check('two_dyn', eval($src) == 12);
}

{
    my $x = 5;
    check('inner_my_shadows', eval('my $x = 9; $x') == 9);
    check('outer_untouched', $x == 5);
}

{
    my %h = (k => 11);
    my $src = '$h{k} += 1; $h{k}';
    check('dyn_hash', eval($src) == 12);
    check('dyn_hash_outer', $h{k} == 12);
}

{
    my @a = (10, 20);
    eval '$a[0] = 99';
    check('dyn_aelem', $a[0] == 99);
}

{
    my $n = 1;
    eval 'sub eval_lex_g { $n++ }';
    check('sub_inc1', eval_lex_g() == 1);
    check('sub_inc2', eval_lex_g() == 2);
    check('sub_inc_outer', $n == 3);
}

{
    my $x = 8;
    my $c = '$x';
    my $fn = sub { eval $c };
    check('closure_eval', $fn->() == 8);
    $x = 15;
    check('closure_eval_mut', $fn->() == 15);
}

{
    my $x = 1;
    my $y = 2;
    check('const_and_dyn', eval('$x') + eval('$y') == 3);
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "eval_lex_deep_done\n";
