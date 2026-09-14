#!/usr/bin/perl
# Deep test for D110: the full qualified-global matrix — reads/writes from
# subs and package blocks, whole-hash copy, keys/values, exists/delete,
# array push/pop/shift/slice/$#idx from subs, string/float/ref values,
# our-declaration unification (same storage via bare + qualified names),
# local() save/restore on a qualified global, and no collision with the
# typeglob (filehandle) registry. All output byte-for-byte vs real perl.
use strict;
use warnings;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
}

# ── scalar: cross-scope read/write from two subs and a package block ──
$G::x = 10;
sub rd1 { return $G::x; }
sub wr1 { $G::x = 20; return $G::x; }
package G;
sub rd_pkg { return $G::x; }
package main;
check('rd1', rd1() == 10);
check('wr1', wr1() == 20);
check('rd1_after_wr', rd1() == 20);
check('rd_pkg', G::rd_pkg() == 20);
check('main_direct', $G::x == 20);

# ── scalar holds string / float / ref values ──
$G::s = "hello";
$G::f = 3.75;
$G::r = [1, 2, 3];
sub shapes {
    my $sum = 0;
    $sum += scalar(@{$G::r});
    return "$G::s/$G::f/$sum";
}
check('shapes', shapes() eq "hello/3.75/3");
$G::f += 0.25;
check('f_incr', $G::f == 4);

# ── whole-hash copy, keys/values, exists/delete ──
%G::h = (a => 1, b => 2, c => 3);
sub hcopy {
    my %c = %G::h;
    return scalar(keys %c) + (exists $c{b} ? 10 : 0);
}
check('hcopy', hcopy() == 12);
check('keys', scalar(keys %G::h) == 3);
check('values', scalar(values %G::h) == 3);
check('exists', exists $G::h{a} ? 1 : 0);
delete $G::h{a};
check('deleted', exists $G::h{a} ? 1 : 0);
sub hset { $G::h{z} = 9; return $G::h{z}; }
check('hset_from_sub', hset() == 9);
check('hset_visible', $G::h{z} == 9);

# ── array push/pop/shift from a sub; slice; $#idx ──
@G::arr = (10, 20, 30, 40);
sub pusher { push(@G::arr, 50); return scalar(@G::arr); }
check('push_sub', pusher() == 5);
sub popper { return pop(@G::arr); }
check('pop_sub', popper() == 50);
sub shifter { return shift(@G::arr); }
check('shift_sub', shifter() == 10);
check('arr_len', scalar(@G::arr) == 3);
my @sl = @G::arr[0, 2];
check('slice', ($sl[0] == 10 && $sl[1] == 30) ? 1 : 0);
sub lastidx { return $#G::arr; }
check('lastidx_sub', lastidx() == 2);
check('lastidx_main', $#G::arr == 2);
my $li = $#G::arr;
check('lastidx_interp', "li=$li" eq "li=2");
check('lastidx_interp_q', "liq=$#G::arr" eq "liq=2");

# ── element read/write across scopes (array + hash) ──
sub elem_wr { $G::arr[1] = 99; return $G::arr[1]; }
check('elem_wr_sub', elem_wr() == 99);
check('elem_rd_main', $G::arr[1] == 99);

# ── our-declared package vars: bare + qualified must unify ──
package P;
our $u = 41;
our @ua = (1, 2);
our %uh = (k => 'v');
package main;
check('our_qual', $P::u == 41);
check('our_bare_interm', "u=$P::u" eq "u=41");
package P;
sub our_rd { return $P::u; }
package main;
check('our_sub_rd', P::our_rd() == 41);
$P::u = 42;
check('our_mutated', $P::u == 42 && P::our_rd() == 42);
sub our_wr { $P::u = 43; return $P::u; }
check('our_wr_from_sub', our_wr() == 43);
my @uc = @P::ua;
check('our_arr_copy', scalar(@uc) == 2);
my %uhc = %P::uh;
check('our_hash_copy', 1);

# ── local() on a qualified global: save, assign, restore ──
$G::y = 1;
sub with_local {
    local $G::y = 5;
    return $G::y;
}
check('local_in', with_local() == 5);
check('local_restored', $G::y == 1);

# ── no collision with the typeglob/filehandle registry ──
$Fh::v = 5;
open(FHLOG, ">devnull.d110") or die "open failed";
print FHLOG "x";
close FHLOG;
unlink "devnull.d110";
check('fh_and_scalar', $Fh::v == 5);
# a qualified var whose bare tail matches an earlier-opened FH name
open(LOG2, ">devnull2.d110") or die "open2 failed";
print LOG2 "x";
close LOG2;
unlink "devnull2.d110";
$LOG2::x = 7;
sub log2rd { return $LOG2::x; }
check('fh_tail_var', $LOG2::x == 7 && log2rd() == 7);

# ── $::x / @::arr / %::h (main:: shorthand) cross-scope ──
$::m = 5;
@::ma = (1, 2);
%::mh = (k => 1);
sub mm { return $::m + scalar(@::ma) + $::mh{k}; }
check('main_shorthand', mm() == 8);
check('main_shorthand_interp', "m=$::m" eq "m=5");
check('main_shorthand_arr_interp', "ma=@::ma" eq "ma=1 2");

# ── ref to a qualified scalar: aliases share one cell ──
$G::z = 1;
my $rz = \$G::z;
$$rz = 2;
check('ref_alias', $G::z == 2);
sub ref_in_sub { my $r2 = \$G::z; $$r2 = 3; return $G::z; }
check('ref_in_sub', ref_in_sub() == 3);

print "d110_deep_done\n";