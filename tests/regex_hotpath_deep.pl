#!/usr/bin/perl
# Deep: boolean match still updates $1/$&/%+; foreach uses live length;
# interpolated/qr/g/s/// and a NUL-in-string match stay correct.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

# postfix if, while, ternary, and/or
my $hits = 0;
$hits++ if "item_9_match" =~ /item_\d+_match/;
$hits++ if "nope" =~ /item_\d+_match/;
check('postfix_if', $hits == 1);

my $w = 0;
my $s = "aaaba";
$w++ while $s =~ /a/g;
check('while_g', $w == 4);

my $t = ("hello" =~ /l+/) ? "yes" : "no";
check('ternary', $t eq "yes");
my $t2 = ("hello" =~ /z+/) ? "yes" : "no";
check('ternary_miss', $t2 eq "no");

check('and_re', 1 && "abc" =~ /b/);
check('or_re', "abc" =~ /z/ || 1);

# $1 / $& after boolean-context match
{
    my ($c1, $amp);
    if ("foobar" =~ /(oo)/) { $c1 = $1; $amp = $&; }
    check('if_dollar1', $c1 eq "oo");
    check('if_amp', $amp eq "oo");
}
{
    my $c1;
    $c1 = $1 if "xyz" =~ /(y)/;
    check('postfix_dollar1', $c1 eq "y");
}

# list-context capture list still works
{
    my @m = ("abc123" =~ /(\d+)/);
    check('list_cap', @m == 1 && $m[0] eq "123");
    my @none = ("abc" =~ /z/);
    check('list_miss', @none == 0);
    my @bare = ("abc" =~ /b/);
    check('list_nogroup', @bare == 1 && $bare[0] == 1);
}

# named %+ after a later unnamed match is empty
"ab" =~ /(?<x>a)(?<y>b)/;
check('plus_hit', $+{x} eq "a" && $+{y} eq "b");
"zz" =~ /z+/;
check('plus_cleared', !defined($+{x}));

# interpolation and qr
my $pat = "item_\\d+_match";
check('interp', "item_6_match" =~ /$pat/);
my $re = qr/item_\d+_match/;
check('qr_bool', "item_6_match" =~ $re);
check('qr_miss', !("item_1" =~ $re));

# s///
{
    my $u = "foo-bar";
    my $n = $u =~ s/-/_/;
    check('subst', $u eq "foo_bar" && $n == 1);
}
{
    my $u = "a1a2a3";
    my $n = $u =~ s/a(\d)/X$1/g;
    check('subst_g_cap', $u eq "X1X2X3" && $n == 3);
}

# split with captures (D126) still interleaves
{
    my @p = split(/(,)/, "a,b,c");
    check('split_cap', join("|", @p) eq "a|,|b|,|c");
}

# NUL in the subject: length-aware match (not strlen)
{
    my $bin = "a" . chr(0) . "b";
    check('nul_a', $bin =~ /a/);
    check('nul_b', $bin =~ /b/);
    check('nul_dot', $bin =~ /a.b/);
}

# foreach live length: push during iteration is seen
{
    my @a = (10);
    my @seen;
    for my $x (@a) {
        push @seen, $x;
        push @a, 20 if $x == 10;
        push @a, 30 if $x == 20;
    }
    check('foreach_chain', join(",", @seen) eq "10,20,30");
}

# /i flag still matches
check('caseless', "AbC" =~ /abc/i);

# boolean false does not clobber a prior $1
"keep" =~ /(keep)/;
"zzz" =~ /nope/;
check('miss_keeps_prior', $1 eq "keep");

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "regex_hotpath_deep_done\n";
