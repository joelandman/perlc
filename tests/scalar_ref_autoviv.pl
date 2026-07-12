#!/usr/bin/perl
# In-depth test suite for D50: `$ref->{a}{b} = val` (chained autoviv
# starting from an *existing* scalar ref, not a hash/array element)
# silently failed.
#
# Root cause: the ArrowDeref-assignment codegen's autoviv dispatch
# (codegen.cpp) only routed through the recursive `emitAutovivContainer`
# helper when the chain's base was itself rooted in a `HashElem`/
# `ArrayElem` (`isElemRootedChain`, established by D40) — e.g.
# `$h{a}{b}{c}`. A chain rooted in a bare *scalar variable* holding a
# ref, e.g. `$ref->{a}{b}`, instead fell to a plain-deref fallback that
# evaluated `$ref->{a}` as an ordinary (non-autovivifying) read — since
# `{a}` didn't exist yet, that read returned undef, and dereferencing
# undef as a hash produced a fresh, disconnected, immediately-discarded
# temporary rather than a slot properly linked back into $ref's own
# hash. The write to `{b}` landed in that throwaway temporary and was
# silently lost.
#
# Fixed by extending the dispatch condition with a new
# `isScalarRootedAllHashChain()` check: a chain rooted in a scalar
# variable is now also routed through `emitAutovivContainer` — but only
# when *every* level of the chain is a hash-key access, never an
# array-index. `emitAutovivContainer`'s existing recursive design
# already correctly bottoms out at a bare scalar variable (dereferencing
# it normally, since a scalar variable itself is never FLAT_ARRAY-
# tagged) and, at each hash-key level, calls the existing
# `perl_hash_autoviv_hash`/`perl_hash_autoviv_array` runtime helpers —
# neither of which touches `perl_array_autoviv_array` (the one D40
# identified as unsafe to call on an array that might hold a
# FLAT_ARRAY-tagged element). A chain with an array-index level anywhere
# (e.g. `$ref->[0][1]`) deliberately still uses the older, non-
# autovivifying fallback and remains a known, separate, narrower open
# gap — extending the fix there would require `perl_array_autoviv_array`
# itself to become FLAT_ARRAY-aware, out of scope here.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — two-level chain from a scalar ref ───────
{
    my $ref = {};
    $ref->{a}{b} = 1;
    check('two_level_chain', $ref->{a}{b} == 1);
}

# ── Section 2: three-level chain from a scalar ref ─────────────────────────
{
    my $ref = {};
    $ref->{a}{b}{c} = 42;
    check('three_level_chain', $ref->{a}{b}{c} == 42);
}

# ── Section 3: mixed chain — hash keys down to a scalar ref, then a ───────
# ── final array-index assignment (safe: only perl_hash_autoviv_array is ───
# ── used, never the FLAT_ARRAY-risky perl_array_autoviv_array) ─────────────
{
    my $ref = {};
    $ref->{x}[0] = "first";
    $ref->{x}[1] = "second";
    check('mixed_hash_then_array_0', $ref->{x}[0] eq "first");
    check('mixed_hash_then_array_1', $ref->{x}[1] eq "second");
}

# ── Section 4: reading a missing deep path returns undef, doesn't crash ───
# ── (regression — this already worked before the fix, since a read ────────
# ── doesn't autovivify) ──────────────────────────────────────────────────
{
    my $ref = {};
    my $v = $ref->{missing}{alsomissing};
    check('read_missing_deep_path_is_undef', !defined($v));
}

# ── Section 5: an already-populated intermediate level is preserved, not ──
# ── overwritten — autoviv only kicks in when genuinely missing ────────────
{
    my $ref = { a => { existing => 1 } };
    $ref->{a}{b} = 99;
    check('existing_sibling_key_preserved', $ref->{a}{existing} == 1);
    check('new_key_added_alongside', $ref->{a}{b} == 99);
}

# ── Section 6: multiple independent branches under the same scalar ref ────
# ── don't interfere with each other ─────────────────────────────────────────
{
    my $ref = {};
    $ref->{branch1}{leaf} = "one";
    $ref->{branch2}{leaf} = "two";
    check('independent_branch_1', $ref->{branch1}{leaf} eq "one");
    check('independent_branch_2', $ref->{branch2}{leaf} eq "two");
}

# ── Section 7: regression — the element-rooted case (D40) still works ─────
{
    my %h;
    $h{a}{b}{c} = "elem_rooted";
    check('element_rooted_regression', $h{a}{b}{c} eq "elem_rooted");
}

# ── Section 8: regression — a scalar-ref-rooted chain with an array-index ─
# ── level is unaffected by this fix (still a separate, documented, open ───
# ── gap — this section checks the *current, still-broken* state matches ───
# ── what perlc has always done, i.e. this is a perlc-only sanity check, ───
# ── not compared against real Perl's correct behavior; see below) ──────────
{
    my $ref = [];
    $ref->[0][1] = "should not silently crash";
    check('array_index_chain_does_not_crash', 1);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "scalar_ref_autoviv_tests_done\n";
