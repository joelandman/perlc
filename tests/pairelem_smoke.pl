#!/usr/bin/perl
# D208 leftover: $z = $row->[$i]; $z->[0] = 9 writes back through a packed
# CPLX_ROW pair. FLOAT_PAIR / empty-row copies stay value-copies.
use strict;
use warnings;

my @fail;
sub check {
    my ( $n, $ok ) = @_;
    print "$n=", ( $ok ? "ok" : "FAIL" ), "\n";
    push @fail, $n unless $ok;
}

{
    my $row;
    $row->[0] = [ 1.0, 2.0 ];
    $row->[1] = [ 3.0, 4.0 ];
    $row->[2] = [ 5.0, 6.0 ];
    my $z = $row->[1];
    $z->[0] = 9.0;
    $z->[1] = 8.0;
    check( 'wb_re', $row->[1][0] == 9.0 );
    check( 'wb_im', $row->[1][1] == 8.0 );
    check( 'wb_z0', $z->[0] == 9.0 );
    check( 'wb_z1', $z->[1] == 8.0 );
    check( 'wb_other0', $row->[0][0] == 1.0 && $row->[0][1] == 2.0 );
    check( 'wb_other2', $row->[2][0] == 5.0 && $row->[2][1] == 6.0 );
}

{
    my $row;
    $row->[0] = [ 1.0, 2.0 ];
    my $z = $row->[0];
    my $w = $z;
    $w->[0] = 7.0;
    $w->[1] = 6.5;
    check( 'alias_z', $z->[0] == 7.0 && $z->[1] == 6.5 );
    check( 'alias_row', $row->[0][0] == 7.0 && $row->[0][1] == 6.5 );
}

{
    sub cadd {
        my ( $a, $b ) = @_;
        return [ $a->[0] + $b->[0], $a->[1] + $b->[1] ];
    }
    my $row;
    $row->[0] = [ 1.0, 2.0 ];
    $row->[1] = [ 3.0, 4.0 ];
    my $n = cadd( $row->[0], $row->[1] );
    $n->[0] = 99.0;
    check( 'cadd_indep', $row->[0][0] == 1.0 && $row->[0][1] == 2.0 );
    check( 'cadd_new',   $n->[0] == 99.0 && $n->[1] == 6.0 );
}

{
    my $row;
    $row->[0] = [ 1.0, 2.0 ];
    $row->[1] = [ 3.0, 4.0 ];
    $row->[1][0] = 5.0;
    check( 'chained', $row->[1][0] == 5.0 && $row->[1][1] == 4.0 );
}

if (@fail) { print "UNEXPECTED_FAILURES=", join( ",", @fail ), "\n"; die "fail\n"; }
print "pairelem_smoke_done\n";
