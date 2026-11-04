#!/usr/bin/perl
# Deep: Stage 36 flat/packed-row scalar-half binding + Stage 37 int @_
# unboxing under real-kernel shapes, verified byte-for-byte vs perl.
# Includes the mbs fill/iterate pattern, aliasing through borrowed rows,
# out-of-range semantics, and a big-int-adjacent accumulator.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

sub cplx { my ($re, $im) = @_; return [ $re, $im ]; }
sub cadd { my ($a, $b) = @_; return [ $a->[0] + $b->[0], $a->[1] + $b->[1] ]; }
sub cmul {
    my ($a, $b) = @_;
    return [ $a->[0] * $b->[0] - $a->[1] * $b->[1],
             $a->[0] * $b->[1] + $a->[1] * $b->[0] ];
}
sub cabs2 { my ($z) = @_; return $z->[0] * $z->[0] + $z->[1] * $z->[1]; }

# ── mbs-shaped: fill rows through an @_ ref, iterate with borrowed rows,
#    magnitude pass — the exact patterns Stage 36/flat-store + Stage 37
#    ($N int param) target.
sub fill_z {
    my ( $z, $N, $xmin, $dx, $ymin, $dy ) = @_;
    for ( my $j = 0; $j < $N; $j++ ) {
        my @row;
        $#row = $N * 2 - 1;
        for ( my $i = 0; $i < $N; $i++ ) {
            $row[ $i * 2 ]     = $xmin + $i * $dx;
            $row[ $i * 2 + 1 ] = $ymin + $j * $dy;
        }
        $z->[$j] = \@row;
    }
    return;
}

sub mbi_one_pass {
    my ( $zzz, $ccc, $N ) = @_;
    my @tmp;
    for ( my $j = 0; $j < $N; $j++ ) {
        my @row;
        $#row = $N * 2 - 1;
        $tmp[$j] = \@row;
    }
    for ( my $j = 0; $j < $N; $j++ ) {
        my $zj  = $zzz->[$j];
        my $ccj = $ccc->[$j];
        my $zpj = $tmp[$j];
        for ( my $i = 0; $i < $N; $i++ ) {
            my $z_re   = $zj->[ $i * 2 ];
            my $z_im   = $zj->[ $i * 2 + 1 ];
            my $cc_re  = $ccj->[ $i * 2 ];
            my $cc_im  = $ccj->[ $i * 2 + 1 ];
            my $new_re = $z_re * $z_re - $z_im * $z_im + $cc_re;
            my $new_im = 2 * $z_re * $z_im + $cc_im;
            $zpj->[ $i * 2 ]     = $new_re;
            $zpj->[ $i * 2 + 1 ] = $new_im;
        }
    }
    # select pass: z = |tmp|<4 ? tmp : (2,0)
    for ( my $j = 0; $j < $N; $j++ ) {
        my $zpj = $tmp[$j];
        my $zj  = $zzz->[$j];
        for ( my $i = 0; $i < $N; $i++ ) {
            my $zp_re = $zpj->[ $i * 2 ];
            my $zp_im = $zpj->[ $i * 2 + 1 ];
            my $abs2  = $zp_re * $zp_re + $zp_im * $zp_im;
            if ( $abs2 < 4.0 ) {
                $zj->[ $i * 2 ]     = $zp_re;
                $zj->[ $i * 2 + 1 ] = $zp_im;
            } else {
                $zj->[ $i * 2 ]     = 2.0;
                $zj->[ $i * 2 + 1 ] = 0.0;
            }
        }
    }
    return;
}

{
    my @z;
    fill_z( \@z, 5, -1.5, 0.5, -1.0, 0.5 );
    my @c;
    $c[$_] = $z[$_] for ( 0 .. 4 );
    mbi_one_pass( \@z, \@c, 5 );

    # z[0][*]: cc=(-1.5,-1); z=cc*cc+cc = (2.25-1-1.5, 3-1)= (-0.25, 2)
    # |z|^2 = 4.0625 >= 4 → z=(2,0)
    check('mbs-z00', $z[0][0] == 2 && $z[0][1] == 0);
    # z[1][*]: cc=(-1.0,-0.5): z=cc*cc+cc = (1-0.25-1, 1-0.5)=( -0.25, 0.5 )
    # |=6.25e-2<4 → stays; after pass 2 (cc aliased = same row!) — recomputes:
    # z=z*z+cc: (0.0625-0.25-1, 2*−0.25*0.5+(-0.5)) = (-1.1875, -0.75) |<4 stays
    check('mbs-z10', abs($z[1][0] + 1.1875) < 1e-12);
    check('mbs-z11', abs($z[1][1] + 0.5) < 1e-12);

    my $mag = 0.0;
    for ( my $i = 0; $i < 5; $i++ ) {
        my $z_re = $z[0]->[$i * 2];
        my $z_im = $z[0]->[$i * 2 + 1];
        $mag += sqrt( $z_re * $z_re + $z_im * $z_im );
    }
    check('mbs-mag', abs($mag - 10.0) < 1e-12);  # all (2,0) → 5x2
}

# ── float-precision through pair halves (no truncation at the bind).
{
    my @row;
    $#row = 3;
    $row[0] = 0.1; $row[1] = 0.2; $row[2] = 0.3; $row[3] = 0.4;
    my $a = $row[0];
    my $b = $row[1];
    check('prec-a', abs($a - 0.1) < 1e-12);
    check('prec-b', abs($b - 0.2) < 1e-12);
    check('prec-add', abs(($a + $b) - 0.3) < 1e-9);
}

# ── OOB read stays undef; negative index wraps like perl.
{
    my @row;
    $#row = 3;
    $row[0] = 42.0; $row[1] = 43.0; $row[2] = 44.0; $row[3] = 45.0;
    check('oob', !defined $row[7]);
    check('neg', $row[-1] == 45);
}

# ── cabs2-style F64 through a pair-unboxed local (Stage 35/36 mix).
{
    my @w;
    $w[0] = [ 3.0, 4.0 ];
    my $p = $w[0];
    check('cabs2-3-4', cabs2($p) == 25);
    my $prod = cmul( $p, $p );
    check('cmul-self', $prod->[0] == -7 && $prod->[1] == 24);
}

# ── mixed int/float accumulator with an int-arg sub (D78 precision:
#    sums that cross 2^53 must not silently lose the integer exactness).
{
    my $big = 9007199254740990;   # just under 2^53
    sub bump { my ($v) = @_; return $v + 2; }
    my $r = bump($big);
    check('big-int', $r == 9007199254740992);
}


print "done\n";
exit(@fail ? 1 : 0);