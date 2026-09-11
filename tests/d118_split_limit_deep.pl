#!/usr/bin/perl
# Deep test for D118: split()'s 3rd LIMIT argument was a hard parse
# error (only 2 args were ever consumed), and neither perl_split nor
# perl_split_regex (src/runtime.c) trimmed trailing empty fields —
# real Perl's default (omitted or zero LIMIT) behavior.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# LIMIT argument, regex separator
{
    my @a = split(/:/, "a:b:c:d", 2);
    check('regex_limit_2', scalar(@a) == 2 && $a[0] eq "a" && $a[1] eq "b:c:d");
}
{
    my @a = split(/:/, "a:b:c", 1);
    check('regex_limit_1', scalar(@a) == 1 && $a[0] eq "a:b:c");
}
# LIMIT argument, plain string separator
{
    my @a = split(",", "a,b,c,d,e", 3);
    check('string_limit_3', scalar(@a) == 3 && $a[2] eq "c,d,e");
}
# LIMIT argument, empty pattern (char split)
{
    my @a = split(//, "hello", 3);
    check('char_split_limit', scalar(@a) == 3 && "@a" eq "h e llo");
}
# LIMIT argument, whitespace split
{
    my @a = split(" ", "a  b  c  d", 2);
    check('whitespace_split_limit', scalar(@a) == 2 && $a[1] eq "b  c  d");
}
# negative LIMIT: unbounded, but do NOT trim trailing empties
{
    my @a = split(/,/, "a,b,,", -1);
    check('negative_limit_no_trim', scalar(@a) == 4);
}
# omitted/zero LIMIT: unbounded, DOES trim trailing empties
{
    my @a = split(/,/, "a,b,,");
    check('omitted_limit_trims', scalar(@a) == 2 && "@a" eq "a b");
    my @b = split(",", "a,b,,");
    check('omitted_limit_trims_string_sep', scalar(@b) == 2);
    my @c = split(/,/, "a,b,,", 0);
    check('explicit_zero_limit_trims', scalar(@c) == 2);
}
# leading empty fields are NOT trimmed (only trailing)
{
    my @a = split(/,/, ",a,b,");
    check('leading_empty_kept', scalar(@a) == 3 && $a[0] eq "");
}
# all-empty-fields string trims down to nothing
{
    my @a = split(/,/, ",,,");
    check('all_empty_trims_to_zero', scalar(@a) == 0);
}
# no match at all: whole string as one field, still subject to trim
# (a non-empty single field is never trimmed)
{
    my @a = split(/x/, "abc");
    check('no_match_single_field', scalar(@a) == 1 && $a[0] eq "abc");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d118_split_limit_done\n";
