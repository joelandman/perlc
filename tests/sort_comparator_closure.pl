#!/usr/bin/perl
# In-depth test suite for D61: sort's { BLOCK } comparator had *zero*
# closure-capture support for any outer block-scoped (non-file-scope)
# lexical variable.
#
# Root cause: sort's custom comparator is compiled as a genuinely
# separate LLVM function (`cmpFn`, codegen.cpp), since it must be passed
# as a real C function pointer to the underlying qsort()-based sort
# routine (`perl_sort_custom`) — unlike map/grep/reduce blocks, which are
# all compiled *inline* in the enclosing function, sharing its scope
# stack directly. Compiling the comparator as its own function requires
# a full scope reset (`scopes_ = {}` etc.) before compiling its body, so
# it previously had no way to see or modify *any* outer block-scoped
# variable at all — only a true file-scope one, via the separate
# fileScalarGlobals_ map that survives the reset (used by the D28 fix's
# $a/$b-shadow check, a different and narrower mechanism). `push
# @observed, $a` inside the comparator silently no-opped when @observed
# was merely declared inside a bare `{ }` block.
#
# Fixed by giving the comparator real closure-capture support, mirroring
# how AnonSub's body already does it: before entering the comparator's
# own scope reset, walk its body to find every scalar name it uses
# (excluding $a/$b, which are bound separately) plus every array/hash
# currently visible in scope, and capture each one's current PerlValue*/
# PerlArray*/PerlHash* via the same perl_make_closure / perl_get_capture
# mechanism a closure body already uses. perl_sort_custom() was extended
# to take a captures array and install it via the same
# s_current_captures thread-local context perl_call_code_ref() already
# uses, so the comparator can call perl_get_capture(idx) exactly as an
# AnonSub body would (save/restore around the qsort() call so a nested
# sort{}/closure call inside the comparator doesn't corrupt it).
#
# IMPORTANT — scope of what this fix covers: arrays and hashes are
# captured *by reference* (a ref-counted wrapper around the same
# underlying struct), so mutating a captured array/hash from inside the
# comparator (push/pop/hash-key-set/etc.) correctly propagates back to
# the caller, and this file tests that extensively. A **separate,
# pre-existing, and NOT fixed here** defect (found and logged as D62
# while writing this file's tests) affects plain *scalar* captures for
# EVERY closure in this codebase (AnonSub bodies too, confirmed with
# plain, sort-unrelated `sub {}` closures, and confirmed pre-existing via
# `git stash`): `perl_array_push_capture()` (runtime.c) clones the
# captured scalar's value instead of storing the original pointer,
# despite its own comment claiming "(no clone)" — so a scalar captured
# by *value* this way can never see or propagate a reassignment
# (`$x = ...`) or increment (`$x++`) made from inside the closure back to
# the caller, or vice versa. This is why every section below that
# exercises "does the comparator see/change an outer variable" uses an
# array or hash target (push / hash-key-assignment), matching D61's own
# literal repro — a plain outer *scalar* mutated by reassignment from
# inside the comparator is a known, separate, open gap (D62), not
# exercised here.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — push() into a block-scoped array from ───
# ── inside the comparator, with a valid (non-degenerate) comparator so ─────
# ── the resulting sort order is also checked exactly ────────────────────────
{
    my @observed;
    my @data = (5, 3, 8, 1, 4);
    my @sorted = sort { push @observed, $a; $a <=> $b } @data;
    check('array_capture_nonempty', scalar(@observed) > 0);
    check('array_capture_sort_order_correct', join(",", @sorted) eq "1,3,4,5,8");
    # algorithm-independent: every observed value must be a real member of
    # the input list, regardless of exactly how many times/with which
    # pairs qsort() invoked the comparator
    my $all_valid = 1;
    for my $v (@observed) {
        my $found = 0;
        for my $d (@data) { $found = 1 if $d == $v; }
        $all_valid = 0 unless $found;
    }
    check('array_capture_values_valid', $all_valid);
}

# ── Section 2: hash capture — the comparator records every value it's ─────
# ── asked to compare into a block-scoped hash ──────────────────────────────
{
    my %seen;
    my @data = (7, 2, 9, 4);
    my @sorted = sort { $seen{$a} = 1; $seen{$b} = 1; $a <=> $b } @data;
    check('hash_capture_sort_order_correct', join(",", @sorted) eq "2,4,7,9");
    my $all_seen = 1;
    for my $d (@data) { $all_seen = 0 unless $seen{$d}; }
    check('hash_capture_all_data_seen', $all_seen);
}

# ── Section 3: two independent captured arrays in the same comparator ─────
{
    my (@lows, @highs);
    my @data = (10, 20, 5, 15);
    my @sorted = sort {
        if ($a < $b) { push @lows, $a; push @highs, $b; }
        else         { push @lows, $b; push @highs, $a; }
        $a <=> $b;
    } @data;
    check('two_array_capture_sort_order', join(",", @sorted) eq "5,10,15,20");
    check('two_array_capture_both_populated',
          scalar(@lows) > 0 && scalar(@highs) > 0);
}

# ── Section 4: regression — a non-capturing sort{} nested in a block ──────
# ── still produces the exact correct order (capture-collection overhead ───
# ── for a comparator that captures nothing must be a no-op) ────────────────
{
    my @data = (9, 1, 6, 3);
    my @sorted = sort { $a <=> $b } @data;
    check('no_capture_regression', join(",", @sorted) eq "1,3,6,9");
}

# ── Section 5: regression — sort{} at true file scope (the pre-existing, ──
# ── already-working case) is unaffected by the new capture machinery ──────
my @file_scope_data = (4, 2, 6, 1);
my @file_scope_sorted = sort { $a <=> $b } @file_scope_data;
check('file_scope_regression', join(",", @file_scope_sorted) eq "1,2,4,6");

# ── Section 6: nested block depth — the captured array is two levels of ───
# ── { } deeper than the sort{} that mutates it ─────────────────────────────
{
    my @outer_log;
    {
        my @data = (9, 2, 7);
        my @sorted = sort { push @outer_log, "$a-$b"; $a <=> $b } @data;
        check('nested_block_sort_order', join(",", @sorted) eq "2,7,9");
    }
    check('nested_block_capture_populated', scalar(@outer_log) > 0);
}

# ── Section 7: the input list itself is also (redundantly) visible to the ─
# ── comparator via over-capture — must not corrupt the sort or crash ──────
{
    my @data = (8, 4, 6, 2);
    my @sorted = sort { $a <=> $b } @data;
    check('over_capture_input_list_safe', join(",", @sorted) eq "2,4,6,8");
    check('over_capture_input_list_unmodified', join(",", @data) eq "8,4,6,2");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "sort_comparator_closure_tests_done\n";
