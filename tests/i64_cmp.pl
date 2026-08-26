# W1 (deep): comparisons and spaceship as VALUES on int vars.
# Exercises the ""-not-"0" false value (perl_alloc_bool) and exact i64
# comparison (latent >2^53 precision fix). Conditions must stay correct.
my $a = 5;
my $b = 7;
my $c = 5;
print(($a < $b) ? "t" : "f", "\n");
print(($a < $b), "\n");
print(($b < $a), "\n");
print("[$a<=$b][$b<=$a][$a==$c][$c!=$a][$a!=$c][$a>=$c]", "\n");
print((10 > 5), (10 > 20), (10 >= 10), (10 <= 10), (10 < 11), (10 != 10), (10 == 10), "\n");
# defined + string form of the false value
my $r = (2 < 1);
print(defined($r) ? "def" : "undef", "\n");
print("[$r]", "\n");
print(length($r), "\n");
print($r eq "" ? "empty" : "notempty", "\n");
print((2 > 1) eq "1" ? "one" : "notone", "\n");
my $rt = (2 > 1);
print("[$rt]", $rt + 0, "\n");
# truthiness in conditions (must remain correct)
if ($a < $b) { print "lt\n"; }
if ($b < $a) { print "never\n"; } else { print "else\n"; }
my $i = 0;
while ($i < 3) { print $i; $i++; }
print "\n";
until ($i > 5) { $i++; }
print($i, "\n");
# spaceship
my $p = 3;
my $q = 9;
print(($p <=> $q), (($p <=> $p)), (($q <=> $p)), "\n");
print(0 <=> 0, 1 <=> 2, 2 <=> 1, "\n");
print((100 <=> 1), (1 <=> 100), "\n");
# large integers > 2^53 — exact i64 compare (boxed double compare loses here)
my $big1 = 9007199254740993;
my $big2 = 9007199254740994;
print(($big1 < $big2) ? "t" : "f", "\n");
print(($big1 == $big2) ? "t" : "f", "\n");
print(($big1 <=> $big2), "\n");
my $big3 = 9007199254740993;
print(($big1 == $big3) ? "t" : "f", "\n");
my $nbig1 = -9007199254740993;
my $nbig2 = -9007199254740994;
print(($nbig1 < $nbig2) ? "t" : "f", "\n");
print(($nbig1 <=> $nbig2), "\n");
# comparisons with computed I64 operands
my $t1 = $a + $b;
my $t2 = $a * 2;
print(($t1 < $t2 + 5) ? "t" : "f", "\n");
print(($t1 & 1) == 0 ? "even" : "odd", "\n");
# string comparisons (boxed path — also fixed to "" false value)
my $s1 = "apple";
my $s2 = "banana";
print(($s1 eq $s2) ? "t" : "f", "\n");
print("[$s1 ne $s2][$s1 lt $s2][$s2 gt $s1][$s1 le $s1][$s1 ge $s1]", "\n");
print("apple" eq "apple", "x", "pear" ne "apple", "x", "a" lt "b", "x", "b" gt "a", "\n");
my $se = ("pear" eq "apple");
print(defined($se) ? "def" : "undef", "\n");
print("[$se]", "\n");
# cmp (string spaceship, boxed — unchanged)
print("apple" cmp "banana", 1, 2, "\n");
