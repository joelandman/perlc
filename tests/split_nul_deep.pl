#!/usr/bin/perl
# Deep test: perl_split (src/runtime.c) fetched a NUL-terminated copy
# of both the string being split and the separator via
# perl_to_string_dup, then scanned them with strlen/strstr/isspace(*p)
# -as-loop-condition — every one of those silently truncates at the
# first embedded NUL byte. Fixed by switching to
# perl_to_string_dup_len's true byte length throughout, with
# index-bounded scanning (memmem instead of strstr) in every branch:
# the string-separator path, the character-split path (empty
# separator), and the whitespace-split path (' '/'\s'/'\s+').
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# string separator: NUL in the data being split
{
    my $s = "a\0b:c\0d:e";
    my @f = split(/:/, $s);
    check('sep_field_count', scalar(@f) == 3);
    check('sep_field0_len', length($f[0]) == 3);
    check('sep_field1_len', length($f[1]) == 3);
    check('sep_field2_len', length($f[2]) == 1);
    check('sep_field0_bytes', join(",", map { ord($_) } split(//, $f[0])) eq "97,0,98");
}

# whitespace-split: NUL in the data
{
    my $s = "a\0b c\0d  e";
    my @w = split(' ', $s);
    check('ws_field_count', scalar(@w) == 3);
    check('ws_field_lens', join("|", map { length($_) } @w) eq "3|3|1");
}

# character-split (empty separator): NUL in the data
{
    my $s = "a\0b\0c";
    my @chars = split(//, $s);
    check('char_split_count', scalar(@chars) == 5);
    check('char_split_nul_char', ord($chars[1]) == 0 && ord($chars[3]) == 0);
}

# separator itself containing NUL bytes
{
    my $s = "aXXbXXc";
    $s =~ s/XX/\0\0/g;
    my @f = split("\0\0", $s);
    check('nul_separator_count', scalar(@f) == 3);
    check('nul_separator_join', join("|", @f) eq "a|b|c");
}

# LIMIT still works correctly with NUL-containing data
{
    my $s = "a\0:b:c:d";
    my @lim = split(/:/, $s, 2);
    check('limit_with_nul_count', scalar(@lim) == 2);
    check('limit_with_nul_field0_len', length($lim[0]) == 2);
    check('limit_with_nul_field1', $lim[1] eq "b:c:d");
}

# regression: ordinary (no-NUL) splits still work exactly as before
{
    my @f = split(/,/, "a,b,,c");
    check('plain_split_unaffected', join("|", @f) eq "a|b||c");
    my @w = split(' ', "  foo   bar  baz ");
    check('plain_ws_split_unaffected', join("|", @w) eq "foo|bar|baz");
    my @c = split(//, "abc");
    check('plain_char_split_unaffected', join("|", @c) eq "a|b|c");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "split_nul_done\n";
