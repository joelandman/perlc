#!/usr/bin/perl
# In-depth test suite for D77: substr() mishandled negative LENGTH and a
# far-out-of-range negative OFFSET.
#
# Real Perl's actual substr(EXPR, OFFSET, LENGTH) algorithm (reverse-
# engineered empirically against real Perl across a 1012-case matrix —
# offset -15..15 x length -15..15 on a 10-char string — plus a separate
# 51-case offset-only sweep for the 2-arg form; both matched perlc's new
# implementation 100% after this fix):
#   start = OFFSET; if start<0, start += length($s)
#   if start > length($s): substr is entirely beyond the string -> undef
#   raw_end = length($s)              if no LENGTH given (2-arg form)
#           = start + LENGTH          if LENGTH >= 0
#           = length($s) + LENGTH     if LENGTH < 0 ("stop N chars before
#                                        the end" - real Perl's own
#                                        documented meaning, previously
#                                        treated by perlc as "no
#                                        truncation at all")
#   if start < 0:
#       if raw_end < 0: window is entirely before the string -> undef
#       start = 0   (otherwise there's partial overlap - clip to 0)
#   clamp raw_end to [start, length($s)]
#   result = substr from start, (raw_end - start) characters
#
# The two originally-reported symptoms:
#   substr($s, 2, -3)     - negative length wrongly returned too much
#   substr($s, -100, 3)   - far-negative offset wrongly clamped instead
#                            of correctly returning undef
#
# The 4-arg replace/lvalue form (substr($s,OFF,LEN,REPL)) uses the same
# bounds algorithm, but real Perl DIES (catchable via eval, target left
# unmodified) rather than returning undef when the window is completely
# out of range - fixed to match.
use strict;
# Several sections below deliberately call substr() with an out-of-range
# offset/length to verify it correctly returns undef (or dies, for the
# 4-arg form) — under `use warnings`, real Perl also emits a "substr
# outside of string" warning in those cases, which perlc doesn't (it has
# no `use warnings` diagnostic system at all, a separate, already-
# documented pre-existing gap — D56). Deliberately NOT using `use
# warnings` here (would need `no warnings 'substr'` to silence just that
# one diagnostic, which hits a separate, previously-undocumented gap
# found while writing this test and logged as new defect D91: `no
# PRAGMA;` — ANY pragma, `no strict;`/`no warnings;` alike, with or
# without arguments — is a hard parse error in perlc, unlike `use
# PRAGMA` which at least parses; not fixed here, out of scope for D77).

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $s = "0123456789";  # length 10, for easy visual verification

# ── Section 1: the original repros ────────────────────────────────────────
{
    check('original_repro_negative_length', substr($s, 2, -3) eq "23456");
    my $r = substr($s, -100, 3);
    check('original_repro_far_negative_offset_is_undef', !defined($r));
}

# ── Section 2: negative LENGTH — "stop N chars before the end" ───────────
{
    check('neg_len_basic', substr($s, 0, -1) eq "012345678");
    check('neg_len_from_offset', substr($s, 3, -2) eq "34567");
    check('neg_len_exact_zero_result', substr($s, 7, -3) eq "");
    check('neg_len_overshoots_to_empty', substr($s, 8, -5) eq "");
    check('neg_len_overshoots_far', substr($s, 0, -100) eq "");
}

# ── Section 3: negative OFFSET, in-range (partial overlap after clip) ────
{
    check('neg_off_partial_overlap_1', substr($s, -12, 3) eq "0");
    check('neg_off_partial_overlap_2', substr($s, -11, 3) eq "01");
    check('neg_off_exact_boundary', substr($s, -10, 3) eq "012");
    check('neg_off_normal', substr($s, -3, 2) eq "78");
}

# ── Section 4: negative OFFSET, entirely out of range -> undef ───────────
{
    check('neg_off_entirely_before_undef_1', !defined(substr($s, -14, 3)));
    check('neg_off_entirely_before_undef_2', !defined(substr($s, -15, 3)));
    check('neg_off_boundary_empty_not_undef', defined(substr($s, -13, 3)));
    check('neg_off_boundary_empty_value', substr($s, -13, 3) eq "");
}

# ── Section 5: positive OFFSET beyond the string -> undef ────────────────
{
    check('pos_off_beyond_undef', !defined(substr($s, 11, 2)));
    check('pos_off_way_beyond_undef', !defined(substr($s, 100, 2)));
    check('pos_off_exact_length_boundary_empty', substr($s, 10, 2) eq "");
    check('pos_off_exact_length_defined', defined(substr($s, 10, 2)));
}

# ── Section 6: 2-arg form (no LENGTH) — negative offset ALWAYS clamps to
#    the start of the string, never returns undef, unlike the 3-arg form ──
{
    check('two_arg_far_negative_offset_full_string', substr($s, -1000) eq "0123456789");
    check('two_arg_normal_negative_offset', substr($s, -3) eq "789");
    check('two_arg_positive_beyond_undef', !defined(substr($s, 100)));
    check('two_arg_exact_boundary_empty', substr($s, 10) eq "");
}

# ── Section 7: large positive LENGTH that overshoots the end still clamps
#    normally (regression — this case already worked before the fix) ─────
{
    check('large_positive_length_clamps', substr($s, 0, 100) eq "0123456789");
    check('large_positive_length_from_offset', substr($s, 5, 100) eq "56789");
}

# ── Section 8: a very negative offset rescued by a large enough positive
#    length still returns the valid, clamped overlap (not undef) ────────
{
    check('far_negative_offset_rescued_by_large_length', substr($s, -1000, 1003) eq "0123456789");
}

# ── Section 9: 4-arg replace/lvalue form — negative length works the same
#    way as the read-only forms ──────────────────────────────────────────
{
    my $t = "0123456789";
    substr($t, 2, -3, "XX");
    check('replace_negative_length', $t eq "01XX789");
}

# ── Section 10: 4-arg replace/lvalue form — completely out-of-range dies
#    (catchable via eval), leaving the target string unmodified ─────────
{
    my $t = "0123456789";
    my $died = 0;
    eval { substr($t, 100, 2, "XX"); };
    $died = 1 if $@;
    check('replace_out_of_range_dies', $died == 1);
    check('replace_out_of_range_unmodified', $t eq "0123456789");
}

# ── Section 11: regressions — ordinary in-range substr (both read and
#    4-arg replace forms) still works correctly, unaffected by this fix ──
{
    check('regression_basic_read', substr($s, 3, 4) eq "3456");
    check('regression_basic_negative_offset', substr($s, -4, 2) eq "67");
    my $t = "abcdefgh";
    substr($t, 2, 3, "XYZ");
    check('regression_basic_replace', $t eq "abXYZfgh");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d77_substr_negative_done\n";
