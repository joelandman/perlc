#!/usr/bin/perl
# Stage 35 deep: alias of the row, mixed non-pair fallback, grow, NaN, -0,
# in-place component write, mandelbrot-shaped inner loop vs real perl.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

sub cplx { my ( $re, $im ) = @_; return [ $re, $im ]; }
sub cadd {
    my ( $a, $b ) = @_;
    return [ $a->[0] + $b->[0], $a->[1] + $b->[1] ];
}
sub cmul {
    my ( $a, $b ) = @_;
    return [
        $a->[0] * $b->[0] - $a->[1] * $b->[1],
        $a->[0] * $b->[1] + $a->[1] * $b->[0]
    ];
}
sub cabs2 {
    my ($z) = @_;
    return $z->[0] * $z->[0] + $z->[1] * $z->[1];
}

# Grow from empty [] through 0..N-1 pair stores
{
    my $row = [];
    for (my $i = 0; $i < 17; $i++) {
        $row->[$i] = cplx( $i * 1.0, -$i * 1.0 );
    }
    check( 'len17', scalar(@{$row}) == 17 );
    check( 'last_re', $row->[16][0] == 16 );
    check( 'last_im', $row->[16][1] == -16 );
}

# Row alias via scalar: $y = $grid->[$j] then write through $y->[$i]
{
    my $grid = [];
    $grid->[0] = [];
    $grid->[0][0] = cplx( 1.0, 2.0 );
    $grid->[0][1] = cplx( 3.0, 4.0 );
    my $y = $grid->[0];
    $y->[0] = cplx( 9.0, 8.0 );
    check( 'alias_row_re', $grid->[0][0][0] == 9.0 );
    check( 'alias_row_im', $grid->[0][0][1] == 8.0 );
}

# Component write $row->[$i][0] = x
{
    my $row = [];
    $row->[0] = cplx( 1.0, 2.0 );
    $row->[0][0] = 5.0;
    check( 'comp_re', $row->[0][0] == 5.0 );
    check( 'comp_im', $row->[0][1] == 2.0 );
}

# Mixed: a non-pair store after pairs still works
{
    my $row = [];
    $row->[0] = cplx( 1.0, 2.0 );
    $row->[1] = "hello";
    check( 'mixed_pair', $row->[0][0] == 1.0 && $row->[0][1] == 2.0 );
    check( 'mixed_str',  $row->[1] eq "hello" );
}

# skip 0/0 — real perl dies "Illegal division by zero" rather than NaN

# Mini mandelbrot-style iteration (N=8, 5 iters) — same shape as bench/mbs.pl
{
    my $N    = 8;
    my $xmin = -1.5;
    my $dx   = 2.5 / $N;
    my $ymin = -1.0;
    my $dy   = 2.0 / $N;
    my $z    = [];
    for (my $j = 0; $j < $N; $j++) {
        $z->[$j] = [];
        for (my $i = 0; $i < $N; $i++) {
            $z->[$j][$i] = cplx( $xmin + $i * $dx, $ymin + $j * $dy );
        }
    }
    my $c = $z;
    my @zp;
    for (my $j = 0; $j < $N; $j++) { $zp[$j] = []; }
    for (my $iter = 0; $iter < 5; $iter++) {
        for (my $j = 0; $j < $N; $j++) {
            my $zj  = $z->[$j];
            my $ccj = $c->[$j];
            my $zpj = $zp[$j];
            for (my $i = 0; $i < $N; $i++) {
                my $p = $zj->[$i];
                $zpj->[$i] = cadd( cmul( $p, $p ), $ccj->[$i] );
            }
        }
        for (my $j = 0; $j < $N; $j++) {
            my $zpj = $zp[$j];
            my $zj  = $z->[$j];
            for (my $i = 0; $i < $N; $i++) {
                my $p = $zpj->[$i];
                if ( cabs2($p) < 4.0 ) { $zj->[$i] = $p; }
                else { $zj->[$i] = cplx( 2.0, 0.0 ); }
            }
        }
    }
    my $esc = 0;
    my $acc = 0.0;
    for (my $j = 0; $j < $N; $j++) {
        my $row = $z->[$j];
        for (my $i = 0; $i < $N; $i++) {
            my $p = $row->[$i];
            $acc += cabs2($p);
            $esc++ if $p->[0] == 2 && $p->[1] == 0;
        }
    }
    check( 'mbi_finite', $acc == $acc && $acc >= 0 );
    print "mbi_escaped=$esc\n";
}

# ref() of a packed row is ARRAY
{
    my $row = [];
    $row->[0] = cplx( 1.0, 2.0 );
    check( 'ref_row', ref($row) eq "ARRAY" );
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "cplx_row_deep_done\n";
