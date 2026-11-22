#!/usr/bin/perl
# PAIR_ELEM deep: write-back, two aliases, half-add via assign, grow, NaN,
# chained still packed, cadd independence, negative half index.
use strict;
use warnings;

my @fail;
sub check {
    my ( $n, $ok ) = @_;
    print "$n=", ( $ok ? "ok" : "FAIL" ), "\n";
    push @fail, $n unless $ok;
}

sub cplx { my ( $re, $im ) = @_; return [ $re, $im ]; }
sub cadd {
    my ( $a, $b ) = @_;
    return [ $a->[0] + $b->[0], $a->[1] + $b->[1] ];
}

{
    my $row;
    for ( my $i = 0 ; $i < 11 ; $i++ ) {
        $row->[$i] = cplx( $i * 1.0, -$i * 1.0 );
    }
    my $z = $row->[7];
    $z->[0] = 70.0;
    $z->[1] = -71.0;
    check( 'grow_wb_re', $row->[7][0] == 70.0 );
    check( 'grow_wb_im', $row->[7][1] == -71.0 );
    check( 'grow_nb6',   $row->[6][0] == 6.0 && $row->[6][1] == -6.0 );
    check( 'grow_nb8',   $row->[8][0] == 8.0 && $row->[8][1] == -8.0 );
}

{
    my $row;
    $row->[0] = cplx( 1.0, 2.0 );
    $row->[1] = cplx( 3.0, 4.0 );
    my $z = $row->[0];
    my $w = $z;
    $z->[0] = 9.0;
    $w->[1] = 8.0;
    check( 'two_z',   $z->[0] == 9.0 && $z->[1] == 8.0 );
    check( 'two_w',   $w->[0] == 9.0 && $w->[1] == 8.0 );
    check( 'two_row', $row->[0][0] == 9.0 && $row->[0][1] == 8.0 );
    check( 'two_s1',  $row->[1][0] == 3.0 && $row->[1][1] == 4.0 );
}

{
    my $row;
    $row->[0] = cplx( 10.0, 20.0 );
    my $z = $row->[0];
    $z->[0] = $z->[0] + 1.0;
    $z->[1] = $z->[1] + 2.0;
    check( 'add_re', $row->[0][0] == 11.0 );
    check( 'add_im', $row->[0][1] == 22.0 );
}

{
    my $row;
    $row->[0] = cplx( 1.0, 2.0 );
    my $z = $row->[0];
    $z->[-1] = 4.5;
    $z->[-2] = 3.5;
    check( 'neg_im', $row->[0][1] == 4.5 );
    check( 'neg_re', $row->[0][0] == 3.5 );
}

{
    my $row;
    $row->[0] = cplx( 1.0, 2.0 );
    $row->[1] = cplx( 3.0, 4.0 );
    $row->[0][0] = 5.0;
    my $z = $row->[1];
    $z->[1] = 9.0;
    check( 'chain_then_alias', $row->[0][0] == 5.0 && $row->[0][1] == 2.0 );
    check( 'chain_alias_im',   $row->[1][1] == 9.0 && $z->[0] == 3.0 );
}

{
    my $row;
    $row->[0] = cplx( 1.0, 2.0 );
    $row->[1] = cplx( 3.0, 4.0 );
    $row->[0] = cadd( $row->[0], $row->[1] );
    check( 'cadd_store_re', $row->[0][0] == 4.0 );
    check( 'cadd_store_im', $row->[0][1] == 6.0 );
    my $n = cadd( $row->[0], $row->[1] );
    $n->[0] = 99.0;
    check( 'cadd_row_stable', $row->[0][0] == 4.0 && $row->[0][1] == 6.0 );
    check( 'cadd_n',          $n->[0] == 99.0 && $n->[1] == 10.0 );
}

{
    my $row;
    $row->[0] = cplx( 1.0, 2.0 );
    my $z = $row->[0];
    check( 'len_z', scalar( @{$z} ) == 2 );
}

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

if (@fail) { print "UNEXPECTED_FAILURES=", join( ",", @fail ), "\n"; die "fail\n"; }
print "pairelem_deep_done\n";
