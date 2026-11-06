#!/usr/bin/env perl
# D198 deep: context propagation through `?:` in every consuming position —
# alias independence, one-branch-only side effects, branch wantarray in both
# directions, nested ternaries, hash-flatten branches, map bodies, list
# unpacking, printf/sprintf args, and the D95 `(f()) x N` guard.
use strict; use warnings;

# alias independence: a list-position ternary must COPY, not alias
my @x = (1,2); my @y = (3,4);
my $c = 1;
my @r = $c ? @x : @y;
$r[0] = 99;
print "X=@x\n";
print "R=@r\n";
my @r2 = 0 ? @x : @y; my @r3 = $c ? [5,6] : (); my @r4 = $c ? () : [7,8];
print "R2=@r2 R3=", scalar(@r3), " R4=", scalar(@r4), "\n";

# only the taken branch evaluates
sub sfx { print "EVAL-$_[0]\n"; return "sfx$_[0]"; }
my @s = $c ? (sfx("then")) : (sfx("else"));
print "S=@s\n";
print "S2=", join(",", $c ? (sfx("t2")) : (sfx("e2"))), "\n";

# branch call sees the ternary's context, not the sub's call context
sub wa { return wantarray ? "L" : "S"; }
my @w = 1 ? (wa()) : ("no");
print "W=@w\n";
my $w2 = 1 ? wa() : "no"; print "W2=$w2\n";
sub wl { return 0 ? (wa()) : ("no"); }
my @w3 = wl(); print "W3=@w3\n";
my $w4 = wl(); print "W4=$w4\n";

# nested ternaries in list position
my @n = 1 ? (0 ? ("a","b") : ("c","d")) : ("e");
print "N=@n\n";
my @n2 = 0 ? (0 ? ("a","b") : ("c","d")) : ("e","f");
print "N2=@n2\n";

# hash-flatten branch (element count only — key order is hash-internal on
# both perlc and perl and not a byte-for-byte-stable shape)
my %h = (p=>5, q=>6);
my @hv = 1 ? %h : ();
print "HV=", scalar(@hv), "\n";

# ternary in a map body (block last-expr)
my @m = map { $_ ? (1,2) : (3) } (1,0,1);
print "M=@m\n";

# list-unpack positions, both arms
my ($u1,$u2) = 1 ? (10,20) : (30,40);
print "U=$u1 $u2\n";
my ($u3,$u4) = 0 ? (10,20) : (30,40);
print "U2=$u3 $u4\n";

# printf/sprintf args flatten like print args
printf "F=%d %d\n", 1 ? (1,2) : (3,4);
my $sf = sprintf "%d|%d", 0 ? (5,6) : (7,8);
print "SF=$sf\n";

# call args flatten (list context at the call boundary)
sub argc { return join("-", @_); }
print "CA=", argc(1 ? (1,2) : (3)), "\n";
print "CA2=", argc(0 ? (1,2) : (3,4)), "|", argc(1 ? (9) : (8,7)), "\n";

# D95 guard: `(f()) x N` must stay string repetition of the scalar value
sub fs { return "ab"; }
my @rp = (fs()) x 2;
print "RP=@rp\n";

# the D198-era row repro: wantarray report inside the sub is still correct
# at every position (the stale-ctx premise did not reproduce — see TESTS.md)
sub wb { return wantarray ? "LIST" : "SCALAR"; }
my ($a1) = wb();
my $a2 = wb();
my @a3 = wb();
print "T1=$a1 T2=$a2 T3=", scalar(@a3), "\n";
printf "T4=%s\n", wb();

# scalar-branch value rules in list position: a scalar branch = 1-element list
my @z1 = 1 ? "x" : (7,8);
print "Z1=@z1\n";
my @z2 = 0 ? (7,8) : "y";
print "Z2=@z2\n";
