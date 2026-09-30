# D155 deep: named unary operators (length lc uc lcfirst ucfirst chr ord hex
# oct abs int sqrt) — precedence against comparison/logical/arithmetic
# operators, $_ defaults in every position, and parenthesized forms.
use strict;
my $x = "AbCd"; my $c = "A"; my $n = 7.9; my $s = "Hi";
print((lc $x eq "abcd") ? "y" : "n");
print((length $x > 3) ? "y" : "n");
print((ord $c == 65) ? "y" : "n");
print((length $x == 4 && 1) ? "y" : "n");
print((defined $x && length $x) ? "y" : "n");
print((abs -3 < 5) ? "y" : "n");
print((int $n >= 7) ? "y" : "n");
print "\n";
print uc $x . "z", "|", hex "ff" + 1, "|", int $n / 2, "|", abs $n - 10, "|",
      length $s x 3, "|", lc $s . "X", "|", ord "a" + 1, "|", chr 65 . "b", "\n";
print((sqrt 16 == 4) ? 1 : 0, (uc $s eq "HI") ? 1 : 0, (lcfirst "ABC" lt "b") ? 1 : 0, "\n");
my @x = ("65", "a", "9");
for my $f (1) {
    print join(",", map { ord } @x), "|", join(",", map { chr ord } @x), "|",
          join(",", map { (length, 1) } @x), "|", join(",", map { uc } @x), "|",
          join(",", map { lc() } @x), "|", join(",", grep { length > 1 } @x), "\n";
}
for ("MiXeD") {
    print lc, "|", uc, "|", lcfirst, "|", ucfirst lc, "|", length, "\n";
    my $l = length;
    my %h = (len => length, first => ord);
    print "$l $h{len} $h{first}\n";
}
for ("ff") { print hex, " ", oct("0x" . $_), "\n"; }
for (-2.5) { print abs, " ", int, "\n"; }
for (81) { print sqrt, "\n"; }
print length("abc"), length(""), (defined length(undef) ? "d" : "u"), "\n";
my @words = qw(pear fig banana);
my @long = grep { length $_ > 3 } @words;
print "@long\n";
my @sorted = sort { length $a <=> length $b or $a cmp $b } @words;
print "@sorted\n";
print "ok\n" if length $x;
print "empty\n" unless length "";
