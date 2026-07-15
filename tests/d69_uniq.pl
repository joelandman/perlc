#!/usr/bin/perl
# In-depth test suite for D69: List::Util::uniq was broken on its two most
# common call shapes.
#
# Root cause #1 (array-variable argument never deduplicated): the parser's
# `uniq` handling correctly recognized `uniq(@arr)` and codegen correctly
# passed @arr's own PerlArray* straight to the runtime's perl_uniq_list()
# (confirmed by inspecting the generated LLVM IR directly) — the bug was
# entirely inside perl_uniq_list() itself (runtime.c): it only stripped
# *consecutive* duplicate elements (Unix `uniq`-command semantics, matching
# its own doc comment "remove consecutive dups"), not real Perl's actual
# List::Util::uniq behavior of keeping the first occurrence of each
# distinct value *anywhere* in the list. For (3,1,4,1,5,9,1), no two
# adjacent elements are equal, so consecutive-dedup left the list
# completely unchanged; it only coincidentally "worked" for inputs like
# (1,1,2,2,3) where every duplicate happens to be adjacent.
#
# Root cause #2 (fully-qualified List::Util::uniq(...) returning empty):
# `uniq`/`sum`/`min`/`max` are recognized only via a dedicated lexer
# keyword for the *bare* identifier — a qualified reference like
# "List::Util::uniq" doesn't match that keyword check at all, so it parsed
# as an ordinary NK::Call with a qualified name, which had no
# special-casing anywhere (unlike POSIX::floor/ceil/fmod, which already
# had this exact "qualified name" interception pattern) and fell through
# to the generic qualified-call codegen, which only knows how to invoke a
# real, separately-compiled LLVM sub — since none of List::Util's builtins
# are that, the call silently produced nothing.
#
# Root cause #3 (uniq's scalar-context return value, found while writing
# this test): real List::Util::uniq in scalar context returns the COUNT of
# unique elements — perlc's pre-existing (and this fix's own first draft)
# code instead returned the *first* unique element's value, a separate,
# smaller bug from the two above, sharing the same code paths.
#
# Fixed by: rewriting perl_uniq_list() (runtime.c) to use a hash-based
# "seen" set (keyed by each element's stringification, matching how real
# Perl's uniq compares values) instead of only comparing to the previous
# element; adding explicit "List::Util::sum"/"min"/"max"/"uniq" name
# interceptions in both CodeGen::emitCall (scalar context) and
# CodeGen::emitArrayPtr (list context, uniq only — sum/min/max are always
# scalar), mirroring the existing POSIX::floor/Time::HiRes::gettimeofday
# precedent already in the same file; and changing uniq's scalar-context
# codegen (both the bare-keyword and newly-added qualified-name paths)
# from perl_array_get(...,0) to perl_array_len(...).
use strict;
use warnings;
use List::Util qw(uniq sum min max);

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original repro — non-consecutive duplicates ────────────
{
    my @u = uniq(3, 1, 4, 1, 5, 9, 1);
    check('non_consecutive_dedup', "@u" eq "3 1 4 5 9");
}
{
    my @arr = (3, 1, 4, 1, 5, 9, 1);
    my @u = uniq(@arr);
    check('array_arg_non_consecutive_dedup', "@u" eq "3 1 4 5 9");
}

# ── Section 2: regression — consecutive duplicates (the one shape that
#    already worked before this fix, via the old buggy implementation) ───
{
    my @u = uniq(1, 1, 2, 2, 3);
    check('consecutive_dedup_regression', "@u" eq "1 2 3");
}

# ── Section 3: already-unique, empty, and single-element lists ────────────
{
    my @u = uniq(1, 2, 3, 4);
    check('already_unique_list', "@u" eq "1 2 3 4");
}
{
    my @u = uniq();
    check('empty_list', scalar(@u) == 0);
}
{
    my @u = uniq(5);
    check('single_element', "@u" eq "5");
}

# ── Section 4: string deduplication (not just numeric) ─────────────────────
{
    my @u = uniq("a", "b", "a", "c", "b");
    check('string_dedup', "@u" eq "a b c");
}

# ── Section 5: scalar context returns the COUNT of unique elements ────────
{
    my $s = uniq(3, 1, 4, 1, 5);
    check('scalar_context_is_count', $s == 4);
}
{
    my @arr = (7, 7, 8, 9, 9, 9);
    my $s = uniq(@arr);
    check('scalar_context_array_arg_count', $s == 3);
}

# ── Section 6: the fully-qualified List::Util::uniq(...) form ──────────────
{
    my @arr = (3, 1, 4, 1, 5, 9, 1);
    my @u = List::Util::uniq(@arr);
    check('qualified_list_context', "@u" eq "3 1 4 5 9");
}
{
    my @arr = (7, 7, 8, 9, 9, 9);
    my $s = List::Util::uniq(@arr);
    check('qualified_scalar_context_count', $s == 3);
}
{
    my @u = List::Util::uniq(1, 1, 2, 2, 3);
    check('qualified_literal_list', "@u" eq "1 2 3");
}

# ── Section 7: fully-qualified sum/min/max also flatten array arguments
#    correctly (same underlying fix, same call-site family) ───────────────
{
    my @arr = (7, 7, 8, 9, 9, 9);
    check('qualified_sum_flattens', List::Util::sum(@arr) == 49);
    check('qualified_min_flattens', List::Util::min(@arr) == 7);
    check('qualified_max_flattens', List::Util::max(@arr) == 9);
}

# ── Section 8: regression — bare (unqualified) sum/min/max unaffected ─────
{
    my @arr = (7, 7, 8, 9, 9, 9);
    check('bare_sum_regression', sum(@arr) == 49);
    check('bare_min_regression', min(@arr) == 7);
    check('bare_max_regression', max(@arr) == 9);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d69_uniq_done\n";
