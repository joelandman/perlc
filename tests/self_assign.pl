#!/usr/bin/perl
# In-depth test suite for scalar/array/hash self-assignment safety.
#
# Found while fixing D35 (Carp::croak bypassing eval): the standard
# error-rethrow idiom `eval {...}; die $@ if $@;` passes $@ to die() as
# the exact same cell it was just written to (perl_get_dollar_at() returns
# the live global's address, not a copy), so `die $@` is effectively
# self-assignment. Testing that surfaced a much more fundamental crash:
# plain `$x = $x;` segfaulted perlc entirely.
#
# Root cause: perl_assign() (and, found by extension while fixing this,
# perl_array_set()/perl_hash_set_str()/perl_hash_set_sv()) all freed the
# assignment target's OLD payload before reading/cloning the new value.
# For ordinary assignment this is fine (dst and src are different cells),
# but for self-assignment dst and src are the SAME pointer — freeing it
# invalidates src too, so the following strdup/clone reads from memory
# that was just freed (or, for array/hash elements, from a slab-pool slot
# that could already be zeroed/reused), producing a crash for perl_assign
# (frees dst->sval, then strdup(src->sval) where src->sval is now NULL)
# or silently wrong garbage values for array/hash elements (the freed slot
# gets cloned into itself, reading unreliable data).
#
# Fixed by making all four functions treat dst==src (or e->val==val for
# the hash/array-element cases) as a guaranteed no-op — self-assignment
# never needs to change anything, so skipping the free+reclone entirely
# is both correct and cheaper.
#
# NOT fixed here (logged as TESTS.md D53, a narrower, separate finding):
# `my $r = \$x; $r = $r;` gives the wrong dereferenced value when this
# happens inside a nested bare `{ }` block (works fine at file/top scope)
# — a scalar-reference-specific self-assignment path, distinct from the
# plain-scalar/array-element/hash-element cases fixed here, and much
# rarer in practice (referencing-then-self-assigning a ref variable,
# specifically inside a block).
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original crash — plain string self-assignment ───────────
{
    my $x = "hello";
    $x = $x;
    check('scalar_string_self_assign', $x eq "hello");
}

# ── Section 2: integer self-assignment ──────────────────────────────────────
{
    my $x = 42;
    $x = $x;
    check('scalar_int_self_assign', $x == 42);
}

# ── Section 3: float self-assignment ────────────────────────────────────────
{
    my $x = 3.14;
    $x = $x;
    check('scalar_float_self_assign', $x == 3.14);
}

# ── Section 4: array-element self-assignment, first and non-first index ────
{
    my @a = (1, 2, 3);
    $a[0] = $a[0];
    check('array_elem_self_assign_first', join(",", @a) eq "1,2,3");
}
{
    my @a = (1, 2, 3);
    $a[1] = $a[1];
    check('array_elem_self_assign_middle', join(",", @a) eq "1,2,3");
}

# ── Section 5: hash-element self-assignment, literal and dynamic key ───────
{
    my %h = (a => 1);
    $h{a} = $h{a};
    check('hash_elem_self_assign_literal_key', $h{a} == 1);
}
{
    my %h;
    my $k = "dyn";
    $h{$k} = 9;
    $h{$k} = $h{$k};
    check('hash_elem_self_assign_dynamic_key', $h{$k} == 9);
}

# ── Section 6: repeated self-assignment in a loop ───────────────────────────
{
    my $x = "abc";
    for (1..3) { $x = $x; }
    check('repeated_self_assign_in_loop', $x eq "abc");
}

# ── Section 7: self-assignment of a package/file-scope global ──────────────
{
    our $g = "global";
    $g = $g;
    check('global_self_assign', $g eq "global");
}

# ── Section 8: self-assignment of an array reference (not a scalar ref — ───
# ── that narrower case is D53, deferred; array refs work at any scope) ─────
{
    my @a = (1, 2, 3);
    my $ar = \@a;
    $ar = $ar;
    check('array_ref_self_assign', join(",", @$ar) eq "1,2,3");
}

# ── Section 9: the idiom that surfaced this bug — die $@ re-throw, where ───
# ── $@ passed to die() is the same live cell it was just assigned into ─────
{
    my $final_msg = "";
    eval {
        eval { die "original\n"; };
        die $@ if $@;
    };
    $final_msg = $@;
    check('die_dollar_at_rethrow', index($final_msg, "original") >= 0);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "self_assign_tests_done\n";
