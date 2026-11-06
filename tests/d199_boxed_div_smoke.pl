#!/usr/bin/env perl
# D199 tail: every division/modulo that reaches the BOXED runtime paths dies
# exactly like real perl — catchable by eval{} with the exact location in $@,
# rc 0 after the catch. Shapes: array-element divisor, string-var divisor,
# sub-call return divisor, boxed %, i64-fast-path %, /= compound-assign,
# %= compound-assign, and the 2D flat-row op / route.
use strict; use warnings;

my @d = (0, 3);
my $x = 0;
eval { $x = 5 / $d[0]; 1 } or print "arr-elem: $@";
print "x=$x\n";

my $s = "0";
eval { my $y = 5 / $s; 1 } or print "str-var: $@";
print "after2\n";

eval { my $m = 5 % $d[0]; 1 } or print "mod-boxed: $@";
my $i2 = 5;
eval { my $m2 = $i2 % 0; 1 } or print "mod-i64: $@";
print "afterm\n";

my $v = 10;
eval { $v /= $d[0]; 1 } or print "comp-div: $@";
print "v=$v\n";
my $w = 7;
eval { $w %= 0; 1 } or print "comp-mod: $@";
print "w=$w\n";

my @rows = ([1.0, 2.0]);
my $z = 0;
eval { $rows[0][1] /= $d[0]; 1 } or print "flatrow-div: $@";
print "row after: $rows[0][1]\n";

sub zd { return 0; }
eval { my $q = 5 / zd(); 1 } or print "sub-ret: $@";
print "afterq\n";

# -0.0 and string -0 divisors die too (ordered == 0.0 comparison)
my $neg = "-0";
eval { my $x = 5 / $neg; 1 } or print "neg0: $@";
eval { my $x = 5 / "0.0"; 1 } or print "str0: $@";

# partially-executed state survives the catch
my $acc = 0;
eval {
    $acc += 1;
    my $t = 1 / $d[0];
    $acc += 100;
    1
} or print "pin: acc=$acc err=$@";
print "acc final: $acc\n";
print "done\n";
