#!/usr/bin/perl
# Boolean =~ / $& / $1 / %+ / foreach-grow: the match hot path.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

my $n = 0;
$n++ if "item_3_match" =~ /item_\d+_match/;
$n++ if "item_1" =~ /item_\d+_match/;
check('bool_if', $n == 1);

check('capture_if', do { my $x; $x = $1 if "hello" =~ /(he)llo/; $x eq "he" });
check('amp_if', do { my $x; $x = $& if "abc" =~ /b/; $x eq "b" });
check('named_plus', do {
    "xy" =~ /(?<a>x)(?<b>y)/;
    $+{a} eq "x" && $+{b} eq "y"
});
check('neg', !("zz" =~ /item_\d+_match/));
check('not_tilde', "zz" !~ /item_\d+_match/);

my @a = (1, 2);
my $c = 0;
for my $x (@a) {
    $c++;
    push @a, 3 if $x == 1;
}
check('foreach_grow', $c == 3 && @a == 3);

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "regex_hotpath_smoke_done\n";
