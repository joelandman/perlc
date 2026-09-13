#!/usr/bin/perl
# Deep test for D126: split() with a capturing-group pattern must include
# each capture's text as extra elements (real Perl's documented behavior).
# perl_split_regex (src/runtime.c) discarded everything between the whole
# match bounds and never looked past ovector slots 0/1, so captured
# delimiter text was silently dropped.
#
# Semantics verified against real perl 5.42 with per-case probes:
# - participating groups interleave in group-number order after the field
#   they preceded (split(/(,)(;)/, "a,;b") -> a , ; b);
# - a group that did not participate in a match still yields an element:
#   UNDEF, not omitted (split(/(x)?,/, "a,b") -> 'a', undef, 'b');
# - named captures (?<n>...) are ordinary capture groups here;
# - LIMIT counts FIELDS only — capture texts are extra elements and never
#   consume the LIMIT budget (split(/(,)/, "a,b,c,d", 2) -> a , "b,c,d");
# - the trailing-empty-field trim (D118, limit==0) also removes trailing
#   UNDEF captures, but never a trailing capture holding a separator
#   (split(/(,)/, "a,") keeps the final ',');
# - plain non-capturing split is unchanged (regression).
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}
sub u { defined $_[0] ? $_[0] : "<undef>" }

# the original repro, rendered distinctly for undef
{
    my @p = split(/(,)/, "a,b,c");
    check('single_group_count', scalar(@p) == 5);
    check('single_group_joined', join("|", map { u($_) } @p) eq "a|,|b|,|c");
}
# multiple capture groups interleave in order
{
    my @q = split(/(,)(;)/, "a,;b");
    check('two_groups_count', scalar(@q) == 4);
    check('two_groups_joined', join("|", map { u($_) } @q) eq "a|,|;|b");
}
# optional/non-participating group -> undef element (not omitted)
{
    my @r = split(/(x)?,/, "a,b");
    check('optional_group_count', scalar(@r) == 3);
    check('optional_group_undef', !defined $r[1] && $r[0] eq "a" && $r[2] eq "b");
    check('optional_group_render', join("|", map { u($_) } @r) eq "a|<undef>|b");
}
# named captures count as capture groups
{
    my @n = split(/(?<n>,)/, "a,b,c");
    check('named_group_count', scalar(@n) == 5);
    check('named_group_joined', join("|", map { u($_) } @n) eq "a|,|b|,|c");
}
# nested capture groups: outer group spans the whole match, inner groups
# each capture their own delimiter
{
    my @nn = split(/((,)(;))/, "a,;b");
    check('nested_groups_count', scalar(@nn) == 5);
    check('nested_groups_joined', join("|", map { u($_) } @nn) eq "a|,;|,|;|b");
}
# LIMIT counts fields only; captures are extra and never consume it
{
    my @l2 = split(/(,)/, "a,b,c,d", 2);
    check('limit2_fields_only', scalar(@l2) == 3);
    check('limit2_last_absorbs', $l2[2] eq "b,c,d");
    check('limit2_capture_kept', $l2[1] eq ",");
    my @l3 = split(/(,)(;)/, "a,;b,;c,;d", 3);
    check('limit3_two_groups', scalar(@l3) == 7);
    check('limit3_joined', join("|", map { u($_) } @l3)
        eq "a|,|;|b|,|;|c,;d");
    my @u3 = split(/(x)?,/, "a,b,c,d", 3);
    check('limit3_optional_groups', scalar(@u3) == 5);
    check('limit3_optional_joined', join("|", map { u($_) } @u3)
        eq "a|<undef>|b|<undef>|c,d");
}
# trailing-empty trim (limit==0) interplay
{
    # trailing '' trimmed, trailing ',' capture kept
    my @t = split(/(,)/, "a,b,,");
    check('trim_keeps_capture', scalar(@t) == 6);
    check('trim_joined', join("|", map { u($_) } @t) eq "a|,|b|,||,");
    # trailing UNDEF captures are trimmed too (real perl: 1 element)
    my @tu = split(/(x)?,/, "a,,");
    check('trim_undef_trailing', scalar(@tu) == 1 && $tu[0] eq "a");
    # negative LIMIT: no trim at all, undef kept
    my @n1 = split(/(x)?,/, "a,,", -1);
    check('neglimit_no_trim', scalar(@n1) == 5);
    check('neglimit_joined',
        join("|", map { u($_) } @n1) eq "a|<undef>||<undef>|");
    # trailing ',,' + captures: trim stops at the ','
    my @t2 = split(/(,)/, "a,b,,", -1);
    check('neglimit_captures', scalar(@t2) == 7);
    # all-empty string with capture: only the last delimiter's capture
    # group is not "trailing", so nothing survives... verify exact shape
    my @t3 = split(/(,)/, ",,");
    check('all_empty_with_captures', scalar(@t3) == 4);
    check('all_empty_joined', join("|", map { u($_) } @t3) eq "|,||,");
}
# leading empty field is kept (only trailing is trimmed)
{
    my @s = split(/(,)/, ",a,b");
    check('leading_empty_kept', scalar(@s) == 5);
    check('leading_joined', join("|", map { u($_) } @s) eq "|,|a|,|b");
}
# capture participating with empty text -> "" element, not undef
{
    my @e = split(/(x?)/, "ab");
    check('empty_capture_count', scalar(@e) == 3);
    check('empty_capture_render', join("|", map { u($_) } @e) eq "a||b");
}
# list-assignment usage (@$_ style), scalar/list context counts
{
    my ($f1, $sep, $f2) = split(/(,)/, "x,y");
    check('list_assign', $f1 eq "x" && $sep eq "," && $f2 eq "y");
}
# regression: plain (non-capturing) split is completely unchanged
{
    my @a = split(/,/, "a,b,c");
    check('plain_split_unchanged', scalar(@a) == 3 && "@a" eq "a b c");
    my @b = split(/,/, "a,b,,");
    check('plain_trim_unchanged', scalar(@b) == 2 && "@b" eq "a b");
    my @c = split(/:/, "a:b:c:d", 2);
    check('plain_limit_unchanged', scalar(@c) == 2 && $c[1] eq "b:c:d");
    my @d = split(/,/, "a,b,,", -1);
    check('plain_neglimit_unchanged', scalar(@d) == 4);
    # string-separator path (perl_split, no regex) is untouched by design
    my @e = split(",", "a,b,c");
    check('string_sep_unchanged', scalar(@e) == 3 && "@e" eq "a b c");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d126_split_captures_done\n";