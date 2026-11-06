use strict;
use warnings;

# Stage 42 deep: the flat-kernel dual-emit (Stage 41) over packed rows plus
# the If-cond reachability fix — kernel-1 (straight pair arithmetic) and
# kernel-2 (if/else uniform branch) certify against packed row variables,
# and the flat and general variants must agree byte-for-byte with perl.

sub fill_row {
    my ($re, $im, $res, $ims, $n) = @_;
    my @r; return \@r if $n <= 0;
    $#r = $n * 2 - 1;
    for my $i (0..$n-1) {
        $r[$i*2]   = $re + $i * $res;
        $r[$i*2+1] = $im + $i * $ims;
    }
    return \@r;
}

# kernel-1 shape: 4 pair-read binds + pure arithmetic + 2 element stores
sub kloop {
    my ($zzz, $pp, $N, $iters) = @_;
    for (my $it = 0; $it < $iters; $it++) {
        for (my $j = 0; $j < $N; $j++) {
            my $zj  = $zzz->[$j];
            my $zpj = $pp->[$j];
            for (my $i = 0; $i < $N; $i++) {
                my $zr = $zj->[$i * 2];
                my $zi = $zj->[$i * 2 + 1];
                my $pr = $zpj->[$i * 2];
                my $pi = $zpj->[$i * 2 + 1];
                my $nr = $zr * $zr - $zi * $zi + $pr;
                my $ni = 2 * $zr * $zi + $pi;
                $zpj->[$i * 2]     = $nr;
                $zpj->[$i * 2 + 1] = $ni;
            }
        }
    }
    return;
}

# kernel-2 shape: pair reads + the abs2 < 4.0 uniform branch
sub kloop_if {
    my ($src, $dst, $N) = @_;
    for (my $j = 0; $j < $N; $j++) {
        my $sp = $src->[$j];
        my $dj = $dst->[$j];
        for (my $i = 0; $i < $N; $i++) {
            my $sp_re = $sp->[$i * 2];
            my $sp_im = $sp->[$i * 2 + 1];
            my $abs2  = $sp_re * $sp_re + $sp_im * $sp_im;
            if ( $abs2 < 4.0 ) {
                $dj->[ $i * 2 ]     = $sp_re;
                $dj->[ $i * 2 + 1 ] = $sp_im;
            } else {
                $dj->[ $i * 2 ]     = 2.0;
                $dj->[ $i * 2 + 1 ] = 0.0;
            }
        }
    }
    return;
}

my @z;
push @z, fill_row(0.0, 0.5 * $_, 0.25, 0.0, 8) for (0..3);
my @p;
for my $j (0..3) { push @p, fill_row(1.0, 1.0, 0.0, 0.0, 8); }

# c-aliased rows: kernel-1 reads BOTH z-aliases (reader/reader sharing of
# one packed buffer is conflict-free) and writes the p rows.
my @c;
for my $j (0..3) { $c[$j] = $z[$j]; }

kloop(\@z, \@p, 4, 3);
print "p0 = $p[0][0] $p[0][1] $p[0][6] $p[0][7]\n";
print "z3 = $z[3][0] $z[3][1]\n";
print "clen = ", scalar(@{$p[0]}), "\n";
print "c-see = $c[0][0] (c must stay 0.0 — the p rows are separate)\n";

kloop_if(\@p, \@z, 4);
print "z0 = $z[0][0] $z[0][1] $z[0][6] $z[0][7]\n";
print "z2 = $z[2][0] $z[2][5]\n";

# single-row arrays (the packed row is the whole value)
my $solo = fill_row(2.0, 1.0, 0.5, 0.25, 4);
print "solo = $solo->[0] $solo->[1] $solo->[6] $solo->[7]\n";
$$solo[2] += 10.0;
print "solo2 = $solo->[2] (in-place element write through the packed row)\n";

# refusal shape: string elements never pack — plain REF hand-out, byte-equal
sub fill_mixed {
    my ($n) = @_;
    my @r; $#r = $n * 2 - 1;
    for my $i (0..$n-1) {
        $r[$i*2]   = $i * 1.5;
        $r[$i*2+1] = ($i == 1) ? "str" : $i * 0.5;
    }
    return \@r;
}
my $mix = fill_mixed(4);
print "mix = $mix->[0] $mix->[1] $mix->[2] $mix->[3]\n";
print "mixref = ", (ref($mix) ? "ref" : "bare"), " len=", scalar(@{$mix}), "\n";

# odd length: the pack refuses, the plain REF stays byte-equal
sub fill_odd {
    my ($n) = @_;
    my @r; $#r = $n - 1;
    for my $i (0..$n-1) { $r[$i] = $i * 1.25; }
    return \@r;
}
my $odd = fill_odd(5);
print "odd = $odd->[0] $odd->[4] len=", scalar(@{$odd}), "\n";

# packflat + push hand-out: pushing a row into an array clones it (share —
# like a REF) and mutation through the pushed copy is visible in the source
sub fill_two { my @r; $#r = 3; $r[0]=1.0; $r[1]=2.0; $r[2]=3.0; $r[3]=4.0; return \@r; }
my $row2 = fill_two();
my @holder;
push @holder, $row2;
$row2->[0] = 40.0;
print "push-alias = $holder[0][0]\n";

# wantarray/list-context return of a row ref stays a ref
sub rref { my @r; $#r = 1; $r[0] = 7.0; $r[1] = 8.0; return \@r; }
my ($a1, $a2) = (rref(), 0);
print "ctx = ", scalar(@$a1), "\n";
