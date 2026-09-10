#!/usr/bin/perl
# Deep: D105 — FLAT_ARRAY / FLOAT_PAIR (Stage 22/23's compact storage for
# anon array-ref literals with 2+ all-numeric elements, e.g. [1,2] or
# [1,2,3]) are references, not values: every alias/clone of one must
# observe writes made through any other alias of the same ref. They used
# to be deep-copied on clone (perl_clone), silently forking the reference
# in two. Root cause and fix: src/runtime.c perl_promote_ref_array()
# (promotes FLAT_ARRAY/FLOAT_PAIR to a real REF_ARRAY in place, reusing
# perl_deref_array's existing lazy-conversion, right before a value that
# might be cloned is read back out — src/codegen.cpp's ScalarVar read
# case, plus the D99 array-to-array copy path). Deliberately does NOT fire
# for fresh literal construction (e.g. numeric matrix rows built directly
# inline, as in tests/d96_flat_row_op_assign.pl / d98_flat_row_2d.pl) —
# those never have a second alias before their first real use, so keeping
# them FLAT_ARRAY/FLOAT_PAIR there costs nothing and preserves the
# Stage 22/23 fast path.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

{
    # FLOAT_PAIR (exactly 2 numeric elements): array-of-refs, write through
    # a separately-named alias, read through the array.
    my $inner = [1, 2];
    my @m = ($inner, [3, 4]);
    $inner->[0] = 55;
    check('float_pair_alias_write_visible', $m[0][0] == 55);
}
{
    # FLAT_ARRAY (3+ numeric elements): same pattern.
    my $inner = [1, 2, 3];
    my @m = ($inner, [9, 9, 9]);
    $inner->[0] = 55;
    check('flat_array_alias_write_visible', $m[0][0] == 55);
    check('flat_array_same_ref', $m[0] == $inner);
}
{
    # D99 interaction: copying an array whose elements are FLAT_ARRAY/
    # FLOAT_PAIR-tagged refs must still preserve those refs' identity.
    my $inner = [1, 2];
    my @a = ($inner, [3, 4]);
    my @b = @a;
    $inner->[0] = 77;
    check('array_copy_then_alias_write', $b[0][0] == 77 && $a[0][0] == 77);
}
{
    # scalar-to-scalar copy of an existing FLAT_ARRAY/FLOAT_PAIR value.
    my $inner = [1, 2];
    my $y = $inner;
    $y->[0] = 99;
    check('scalar_copy_alias', $inner->[0] == 99);
}
{
    # push() onto an existing array (not a list literal).
    my $r = [10, 20, 30];
    my @arr;
    push @arr, $r;
    $r->[0] = 77;
    check('push_alias', $arr[0][0] == 77);
}
{
    # passed as a sub argument, mutated inside the sub.
    sub touch { my ($x) = @_; $x->[0] = 55; }
    my $r = [1, 2];
    touch($r);
    check('sub_arg_alias', $r->[0] == 55);
}
{
    # stored as a hash value.
    my %h;
    my $r = [1, 2, 3];
    $h{k} = $r;
    $r->[0] = 88;
    check('hash_value_alias', $h{k}[0] == 88);
}
{
    # fresh literal rows (no separate named alias) must still work byte
    # for byte — this is the Stage 22/23 fast path itself, unaffected by
    # the D105 fix. Mirrors tests/d96_flat_row_op_assign.pl's shape.
    my @P = ([1.0, 2.0, 3.0], [4.0, 5.0, 6.0]);
    for my $i (0 .. 1) {
        for my $k (0 .. 2) {
            $P[$i][$k] += 0.5;
        }
    }
    check('fresh_literal_rows_unaffected',
        $P[0][0] == 1.5 && $P[0][1] == 2.5 && $P[1][2] == 6.5);
}
{
    # ref() must still report ARRAY regardless of internal tag or
    # promotion state (D83).
    my $y = [4, 5, 6];
    my @z = ($y);
    check('ref_still_array', ref($y) eq 'ARRAY' && ref($z[0]) eq 'ARRAY');
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d105_flat_ref_alias_done\n";
