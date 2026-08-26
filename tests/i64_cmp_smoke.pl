# W1 (smoke): comparisons as values
my $a = 3;
my $b = 9;
print(($a < $b), ($b < $a), ($a == $a), "\n");
print(($a <=> $b), ($b <=> $a), ($a <=> $a), "\n");
print("x[$a gt $b]y", "\n");
print("apple" eq "apple", "x", "apple" ne "pear", "x", "a" lt "b", "\n");
