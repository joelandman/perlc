#!/usr/bin/perl
# Deep: REF-base element stores through promoted @_ params + aliasing +
# mixed pair/flat/scalar stores, compared for real Perl semantics.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

# cplx-style row construction through an @_ ref (the mbs fill_z shape), then
# an in-place iteration pass over the rows — the exact pattern the Stage-35
# fast paths optimize.
sub cplx    { my ($re, $im) = @_; return [ $re, $im ]; }
sub cadd    { my ($a, $b) = @_; return [ $a->[0] + $b->[0], $a->[1] + $b->[1] ]; }
sub cmul    {
    my ($a, $b) = @_;
    return [ $a->[0] * $b->[0] - $a->[1] * $b->[1],
             $a->[0] * $b->[1] + $a->[1] * $b->[0] ];
}
sub cabs2   { my ($z) = @_; return $z->[0] * $z->[0] + $z->[1] * $z->[1]; }

sub fill_pairs_ref {
    my ($z, $n, $base) = @_;
    for (my $i = 0; $i < $n; $i++) {
        $z->[$i] = cplx($base + $i * 0.5, -($base + $i * 0.5));
    }
}

# rows made through a ref, iterated via borrowed element PVs
{
    my @z;
    fill_pairs_ref(\@z, 6, 1.0);
    my @w;
    fill_pairs_ref(\@w, 6, 2.0);

    # element-store update: w = z*z + w — through borrowed row PVs.
    # (Uses temporary pair variables; chained `cadd(...)->[0]` projection on a
    # call result hits a separate pre-existing list-index vs arrow-deref
    # ambiguity, tracked separately from the REF-base store fix.)
    for (my $i = 0; $i < 6; $i++) {
        my $zi = $z[$i];
        my $wi = $w[$i];
        my $prod = cmul($zi, $zi);
        my $sum  = cadd($prod, $wi);
        $wi->[0] = $sum->[0];
        $wi->[1] = $prod->[1] + $wi->[1];
    }
    # w[0] = (1,-1)*(1,-1) + (2,-2) = (0,-2)+(2,-2) = (2,-4)
    check('iter-re', $w[0][0] == 2);
    check('iter-im', $w[0][1] == -4);
    check('count-z', scalar(@z) == 6);
    check('count-w', scalar(@w) == 6);
}

# scalar-rooted single-level chain through a ref to a shared outer — D50 path
{
    my @a;
    my $r  = \@a;
    $r->[0] = [ 1, 2 ];
    my @b = ( $r->[0] );
    check('chain-count', scalar(@b) == 1);
    check('chain-im',    $b[0][1] == 2);
    $r->[0][0] = 7.5;                 # LVALUE write through the same chain
    check('chain-lv',    $a[0][0] == 7.5);
}

# fill through a ref, then verify @-flattening sees the stored pairs
{
    sub fill_ret {
        my ($z) = @_;
        $z->[0] = [ 1.0, 2.0 ];
        $z->[1] = [ 3.0, 4.0 ];
        return scalar(@$z);
    }
    my @g;
    my $n = fill_ret(\@g);
    check('ret-n',   $n == 2);
    check('flat-join', join(':', map { $_->[0] } @g) eq '1:3');
}

# hash-element / array-element bases keep their (previously working) path
{
    my %h;
    $h{k}[0] = [ 1.5, 2.5 ];
    check('hashelem', $h{k}[0][1] == 2.5);

    my @rows;
    $rows[0][0] = [ 7.5, 8.5 ];
    check('arrayelem', $rows[0][0][1] == 8.5);
}

# cabs2-style F64 reads from rows stored through a ref
{
    my @z;
    fill_pairs_ref(\@z, 4, 0.0);
    my $m = 0.0;
    for (my $i = 0; $i < 4; $i++) {
        my $v = cabs2($z[$i]);
        $m += $v;
    }
    # z[i] = (0.5i, -0.5i): abs2 = 2*(0.5i)^2 = 0.5 i^2 → sum i=0..3 = 0.5*(0+1+4+9)=7
    check('f64-mag', $m == 7);
}

print "done\n";
exit(@fail ? 1 : 0);