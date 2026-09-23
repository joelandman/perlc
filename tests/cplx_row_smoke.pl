#!/usr/bin/perl
# Stage 35: packed complex rows — $row->[$i] = [re,im] stays a 2-elem arrayref.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

{
    my $z = [];
    $z->[0] = [];
    $z->[0][0] = [ 1.5, 2.5 ];
    $z->[0][1] = [ 3.0, 4.0 ];
    check('re0', $z->[0][0][0] == 1.5);
    check('im0', $z->[0][0][1] == 2.5);
    check('re1', $z->[0][1][0] == 3.0);
    check('im1', $z->[0][1][1] == 4.0);
}

{
    my $row = [];
    for (my $i = 0; $i < 8; $i++) {
        $row->[$i] = [ $i * 1.0, $i + 0.5 ];
    }
    my $s = 0.0;
    for (my $i = 0; $i < 8; $i++) {
        my $p = $row->[$i];
        $s += $p->[0] + $p->[1];
    }
    check('sum8', $s == 32);
}

{
    sub cadd {
        my ( $a, $b ) = @_;
        return [ $a->[0] + $b->[0], $a->[1] + $b->[1] ];
    }
    my $row = [];
    $row->[0] = [ 1.0, 2.0 ];
    $row->[1] = [ 3.0, 4.0 ];
    $row->[0] = cadd( $row->[0], $row->[1] );
    check('cadd_re', $row->[0][0] == 4.0);
    check('cadd_im', $row->[0][1] == 6.0);
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "cplx_row_smoke_done\n";
