#!/usr/bin/env perl
# D97: Math::BigInt built-in using mini-gmp — smoke test
# Tests: new, stringification, +, -, *, /, <=>, neg, bmul, badd, bsub, bcmp, numify
use Math::BigInt;

my $x = Math::BigInt->new(123);
my $y = Math::BigInt->new(456);

# stringification
my $s = "$x";
print "str=$s\n";

# arithmetic via overload
my $z = $x + $y;
print "add=$z\n";
my $d = $y - $x;
print "sub=$d\n";
my $m = $x * $y;
print "mul=$m\n";
my $q = $y / $x;
print "div=$q\n";

# comparison via <=> overload
print "eq=", ($x == $x ? 1 : 0), "\n";
print "ne=", ($x != $y ? 1 : 0), "\n";
print "lt=", ($x <  $y ? 1 : 0), "\n";
print "gt=", ($x >  $y ? 1 : 0), "\n";

# in-place mutators
my $a = Math::BigInt->new(10);
$a->bmul(Math::BigInt->new(3));
print "bmul=$a\n";
$a->badd(Math::BigInt->new(5));
print "badd=$a\n";
$a->bsub(Math::BigInt->new(3));
print "bsub=$a\n";

# chaining (bmul->badd)
my $b = Math::BigInt->new(2);
$b->bmul(Math::BigInt->new(3))->badd(Math::BigInt->new(4));
print "chain=$b\n";

# bcmp
my $c = Math::BigInt->new(100);
my $cmp = $c->bcmp(Math::BigInt->new(50));
print "bcmp=$cmp\n";

# neg
my $n = Math::BigInt->new(42);
my $neg = -$n;
print "neg=$neg\n";

# large number (beyond i64)
my $big = Math::BigInt->new(10);
for my $i (1..20) { $big->bmul(Math::BigInt->new(10)); }
print "big=$big\n";

# numify
my $num = Math::BigInt->new(99);
print "numify=", $num->numify(), "\n";