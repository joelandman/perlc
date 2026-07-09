#!/usr/bin/perl
# In-depth test suite for zero-width regex substitution matches.
#
# Root cause of the original crash (`s/$/text/` on any string): PCRE2's
# match-start argument (`pos`) is where the search *begins*, not necessarily
# where the match *lands* — an anchor like `$` can match ahead of `pos` (at
# end-of-string). perl_regex_subst's zero-length-match handling conflated
# the two: it copied `s[pos]` (already-copied by the preceding "text before
# match" step, since `pos` is behind the real match point) instead of
# `s[mstart]`, and — critically — set `pos = mstart + 1`, which for a match
# exactly at end-of-string becomes `slen + 1`. The subsequent `slen - pos`
# (both `size_t`) then underflowed to a value near SIZE_MAX, and the
# resulting "copy ~18 quintillion remaining bytes" memcpy corrupted the
# heap — the segfault surfaced later, inside pcre2_match_data_free(), once
# the corruption reached PCRE2's own bookkeeping.
#
# Fixed by using `mstart` (not `pos`) for the zero-width-match character
# copy, and clamping the final `slen - pos` computation so it can't
# underflow when `pos` ends up one past the end of the string.
#
# NOTE: `foreach my $x (@arr) { $x =~ s/.../ }` is deliberately NOT used
# here to exercise mutation-through-iteration — foreach doesn't alias its
# loop variable to the source array yet (TESTS.md D37, a separate, already
# tracked bug) — array-element substitution (`$arr[$i] =~ s/.../`) is used
# instead so this suite stays focused on the zero-width-match fix.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original crash — end-anchor, non-global ─────────────────
{
    my $s = "hello";
    $s =~ s/$/world/;
    check('end_anchor_nonglobal', $s eq "helloworld");
}

# ── Section 2: end-anchor, global ───────────────────────────────────────────
{
    my $s = "hello";
    $s =~ s/$/!/g;
    check('end_anchor_global', $s eq "hello!");
}

# ── Section 3: start-anchor, non-global (regression) ────────────────────────
{
    my $s = "hello";
    $s =~ s/^/X/;
    check('start_anchor_nonglobal', $s eq "Xhello");
}

# ── Section 4: start-anchor, global ─────────────────────────────────────────
{
    my $s = "hello";
    $s =~ s/^/X/g;
    check('start_anchor_global', $s eq "Xhello");
}

# ── Section 5: empty-string subject, end anchor ─────────────────────────────
{
    my $s = "";
    $s =~ s/$/Y/;
    check('empty_subject_end_anchor', $s eq "Y");
}

# ── Section 6: empty-string subject, start anchor ───────────────────────────
{
    my $s = "";
    $s =~ s/^/Y/;
    check('empty_subject_start_anchor', $s eq "Y");
}

# ── Section 7: single-character subject, end anchor ─────────────────────────
{
    my $s = "x";
    $s =~ s/$/!/;
    check('single_char_end_anchor', $s eq "x!");
}

# ── Section 8: single-character subject, start anchor ───────────────────────
{
    my $s = "x";
    $s =~ s/^/!/;
    check('single_char_start_anchor', $s eq "!x");
}

# ── Section 9: zero-width lookahead in the middle, global ──────────────────
{
    my $s = "abc";
    $s =~ s/(?=b)/-/g;
    check('lookahead_middle_global', $s eq "a-bc");
}

# ── Section 10: zero-width lookahead in the middle, non-global ─────────────
{
    my $s = "abc";
    $s =~ s/(?=b)/-/;
    check('lookahead_middle_nonglobal', $s eq "a-bc");
}

# ── Section 11: empty pattern — zero-width match at every position, global ─
{
    my $s = "abc";
    $s =~ s//-/g;
    check('empty_pattern_every_position', $s eq "-a-b-c-");
}

# ── Section 12: multiline end-anchor with /m and /g ─────────────────────────
{
    my $s = "ab\ncd";
    $s =~ s/$/!/mg;
    check('multiline_end_anchor_global', $s eq "ab!\ncd!");
}

# ── Section 13: end-anchor substitution on a string containing a newline, ──
# ── no /m (Perl's $ without /m matches at end-of-string, or just before a ──
# ── single trailing newline — here there's an embedded, not trailing, \n) ──
{
    my $s = "a\nb";
    $s =~ s/$/E/;
    check('embedded_newline_no_m', $s eq "a\nbE");
}

# ── Section 14: return value is the substitution count, not the string ─────
{
    my $s = "abc";
    my $n = ($s =~ s/$/X/);
    check('return_value_is_count', $n == 1 && $s eq "abcX");
}

# ── Section 15: non-zero-length substitution still works (regression) ──────
{
    my $s = "hello world";
    $s =~ s/world/perl/;
    check('normal_substitution_regression', $s eq "hello perl");
}
{
    my $s = "aaa";
    $s =~ s/a/b/g;
    check('normal_global_substitution_regression', $s eq "bbb");
}

# ── Section 16: zero-width match via array-element substitution ────────────
{
    my @arr = ("hi", "yo");
    for (my $i = 0; $i < scalar(@arr); $i++) { $arr[$i] =~ s/$/!/; }
    check('array_elem_end_anchor', join(",", @arr) eq "hi!,yo!");
}

# ── Section 17: anchored character match (not purely zero-width) ──────────
{
    my $s = "hello";
    $s =~ s/o$/O/;
    check('anchored_nonzero_match', $s eq "hellO");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "regex_subst_zero_width_tests_done\n";
