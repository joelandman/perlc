#!/usr/bin/perl
# Deep test for D106: same bug class as D105 (a FLAT_ARRAY/FLOAT_PAIR-
# tagged anon-array-ref — Stage 22/23's compact storage for `[1,2]`-
# style literals — silently forking into two independent arrays when
# aliased a second time), narrowed to reading the ref back out of an
# ARRAY or HASH element specifically (`$arr[0]`, `$h{k}`) rather than a
# plain scalar variable, which D105 already covered.
#
# Root cause: the scalar-variable read path (case NK::ScalarVar in
# emitExpr) already called perl_promote_ref_array before handing out
# its value (D105's fix), but the analogous ArrayElem/HashElem read
# paths in emitExpr did not — so `my $y = $arr[0]; $y->[0] = 99;`
# still forked. Fix: both `case NK::ArrayElem` and `case NK::HashElem`
# in emitExpr now call perl_promote_ref_array on the value before
# returning it, exactly mirroring D105's ScalarVar fix. Confirmed this
# does NOT touch the `$arr[$i] op= rhs` / `$hash{key} op= rhs`
# compound-assign fast paths, nor any 2D ArrowDeref-chain fast path —
# all of those are separate case/dispatch branches that call
# perl_array_get_ref / emitHashGetRef directly themselves, not through
# this one shared read path.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# array element alias
my @arr = ([1, 2, 3]);
my $ay = $arr[0];
$ay->[0] = 99;
check('array_elem_alias_sees_write', $arr[0][0] == 99);

# hash element alias
my %h = (k => [10, 20, 30]);
my $hy = $h{k};
$hy->[0] = 77;
check('hash_elem_alias_sees_write', $h{k}[0] == 77);

# the write must be visible in BOTH directions: mutating through the
# original array/hash element must also be visible via the alias
$arr[0][1] = 55;
check('array_elem_write_visible_via_alias', $ay->[1] == 55);
$h{k}[1] = 66;
check('hash_elem_write_visible_via_alias', $hy->[1] == 66);

# regression guard: the 2D compound-assign fast path (a totally
# separate codegen branch) must be unaffected by this fix
my @P = ([1, 2, 3], [4, 5, 6], [7, 8, 9]);
for my $i (0 .. 2) {
    for my $k (0 .. 2) {
        $P[$i][$k] += 10;
    }
}
check('twod_compound_assign_unaffected',
    join(";", map { join(",", @$_) } @P) eq "11,12,13;14,15,16;17,18,19");

my $ref = [[1, 2], [3, 4]];
$ref->[0][1] *= 5;
check('twod_arrowderef_compound_unaffected', $ref->[0][1] == 10);

my %hh = (row => [1, 2, 3]);
$hh{row}[1] += 100;
check('hash_2d_compound_unaffected', join(",", @{ $hh{row} }) eq "1,102,3");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d106_flat_ref_elem_alias_done\n";
