# D155 smoke: named unary operators bind tighter than comparison
# (`length $x > 3` was length($x > 3)) and default to $_ when bare
# (`map { ord } @x` was a parse error).
my $x = "AbCd";
print((length $x > 3) ? "y" : "n", (lc $x eq "abcd") ? "y" : "n", "\n");
print join(",", map { ord } split //, "AB"), "\n";
for ("Zed") { print lc, uc, length, "\n"; }
