# D98: FLAT_ARRAY 2D rows must survive writes without being corrupted.
# A row that is an all-numeric anon array ([1.0,2.0,3.0]) compiles to a
# PERL_FLAT_ARRAY (pval = double[], tag 10).  Two write paths used to break
# that representation, and the 2D read fast-path (which bakes in
# llvm.assume(tag==FLAT_ARRAY)) then read a stale/garbage pointer and segfaulted.
#
# Section 1 — adjacent-form compound-assign:  $P[$i][$k] += ...
#   The inner $P[$i] parses to an ArrayElem (not an ArrowDeref), so the 2D
#   fast-path check (which requires ArrowDeref) was skipped and the write fell
#   through to a branch that called perl_deref_array_ro(row) on a FLAT_ARRAY,
#   returning the double[] as a PerlArray*  ->  segfault.
# Section 2 — plain 2D assignment into a flat row, then reading it back:
#   $b->[0][1] = 9.0  used to route through perl_array_autoviv_array_from_scalar,
#   which flips an existing FLAT_ARRAY row to a REF_ARRAY (*slot = ref).  A
#   later fast-path read (the energy loop) then loaded the converted row's
#   pval (a PerlArray*) as a double*  ->  segfault.  The value must also
#   actually persist to the row (row0_1 == 9.0).

my @P = ([1.0,2.0,3.0],[4.0,5.0,6.0],[7.0,8.0,9.0]);
for my $i (0..2) {
  for my $k (0..2) {
    $P[$i][$k] += 0.5;
  }
}
printf("adj=%.3f %.3f %.3f\n", $P[0][0], $P[1][1], $P[2][2]);

my $b = [ [1.0,2.0,3.0], [4.0,5.0,6.0] ];
my $px = 0.0;
for my $i (0..1) { $px += $b->[$i][1] * $b->[$i][2]; }
$b->[0][1] = 9.0;
my $e = 0.0;
for my $i (0..1) { $e += $b->[$i][2] ** 2; }
printf("px=%.3f e=%.3f row0_1=%.3f\n", $px, $e, $b->[0][1]);
