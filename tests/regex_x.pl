#!/usr/bin/perl
# Deep: /x comments, alt delimiters, split, captures, /xi, /g.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

check('mparen_x', "abc" =~ m( a b c )x);
check('mslash_x', "abc" =~ m/ a b c /x);
check('xi', "Hello" =~ / h e l l o /xi);
check('cap', do { "abcdef" =~ / a (b c) d /x; $1 eq "bc" });
check('split_x', join(",", split(/ \s+ /x, "a  b   c")) eq "a,b,c");
check('hash_comment', "ac" =~ /a#foo
c/x);
check('class_space_kept', "a b" =~ /[a b]/x);  # /x does not unspace []
{
    my $t = "aabbcc";
    my $n = 0;
    $n++ while $t =~ / (a|b|c) \1 /xg;
    check('g_x', $n == 3);
}
{
    my $s = "Hello World";
    $s =~ s{ Hello \s+ World }{OK}x;
    check('s_brace_x', $s eq "OK");
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "regex_x_done\n";
