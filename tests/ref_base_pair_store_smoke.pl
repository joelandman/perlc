#!/usr/bin/perl
# REF-base element store: my ($z) = @_; $z->[i] = [pair] (and my $ref = \@a;
# $ref->[0] = [pair]) must land in the OUTER container — perl_array_set_row
# used to pack-convert the ref PV in place, silently dropping the store.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

sub fill_pairs {
    my ($z) = @_;
    for (my $j = 0; $j < 3; $j++) {
        $z->[$j] = [ $j * 1.0, -($j * 1.0) ];
    }
}

{
    my @a;
    fill_pairs(\@a);
    check('count', scalar(@a) == 3);
    check('r00',   $a[0][0] == 0);
    check('r01',   $a[0][1] == 0);
    check('r10',   $a[1][0] == 1);
    check('r11',   $a[1][1] == -1);
    check('r20',   $a[2][0] == 2);
    check('r21',   $a[2][1] == -2);
}

{
    my @a;
    my $ref = \@a;
    $ref->[0] = [ 3.5, 4.5 ];
    check('plainref1', $a[0][1] == 4.5);
    check('plainref-n', scalar(@a) == 1);
}

{
    my @d;
    sub fill_var {
        my ($z) = @_;
        my $y = [ 3, 4 ];
        $z->[0] = $y;
    }
    fill_var(\@d);
    check('var-count', scalar(@d) == 1);
    check('var-im',    $d[0][1] == 4);
}

# aliased row ref stays consistent under later writes through the alias
{
    my @a;
    fill_pairs(\@a);
    my $alias = \@a;
    $alias->[1] = [ 9.5, 8.5 ];
    check('alias-im', $a[1][1] == 8.5);
    check('alias-re', $a[1][0] == 9.5);
    check('alias-n',  scalar(@a) == 3);
}

# a flat string store through the same ref shape keeps working
{
    sub fill_scalar {
        my ($z) = @_;
        $z->[0] = 5.5;
    }
    my @s;
    fill_scalar(\@s);
    check('scalar', $s[0] == 5.5);
}

# mix: rows created through a ref, then flat-indexed like bench/mbs.pl does
{
    sub flat_fill_pair_stub {
        my ($re_base, $im_base, $re_stride, $im_stride, $n) = @_;
        my @r;
        return \@r if $n <= 0;
        $#r = $n * 2 - 1;
        for my $i (0 .. $n - 1) {
            $r[ $i * 2 ]     = $re_base + $i * $re_stride;
            $r[ $i * 2 + 1 ] = $im_base + $i * $im_stride;
        }
        return \@r;
    }
    sub fill_z_local {
        my ($z, $N, $xmin, $dx, $ymin, $dy) = @_;
        for (my $j = 0; $j < $N; $j++) {
            $z->[$j] = flat_fill_pair_stub($xmin, $ymin + $j * $dy, $dx, 0, $N);
        }
    }
    my @z;
    fill_z_local(\@z, 4, -1.5, 0.5, -1.0, 0.5);
    check('mbsfill-n',  scalar(@z) == 4);
    check('mbsfill-re', $z[0][2] == -1.0);
    check('mbsfill-im', $z[0][3] == -1.0);
    check('mbsfill-im2', $z[3][7] == 0.5);
}

print "done\n";
exit(@fail ? 1 : 0);