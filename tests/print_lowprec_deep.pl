# D147/D148 deep: print/say/printf arguments stop at low-precedence and/or
# (list-operator precedence); the print is the LHS and yields true.
# `reverse` in a print/say/sprintf argument list is list reversal.
use strict;
use warnings;
use feature 'say';

my @a = (1, 2, 3);
my %h = (k => 'v');
my $n = 0;

# ---- low-precedence and/or after print/say/printf ----
sub bump { $n++; return $_[0] }

print "a" and bump(1); print " n=$n\n";
print "b" or  bump(1); print " n=$n\n";
say "c" and bump(0) or print "d\n";
say "e" and bump(1) or print "never\n";
printf("%d-", 7) and print "f\n";
printf STDOUT "%s-", "g" and print "h\n";
print STDERR "" and print "i\n";

for my $i (1 .. 3) {
    print "loop$i" and next if $i == 2;
    print " body$i";
}
print "\n";

sub f {
    my $x = shift;
    print "f($x)" and return "early" if $x;
    return "late";
}
print " ", f(1), "\n";
print " ", f(0), "\n";

my $V = "val";
print $V and print " after\n";
print $V or die "unreachable";
print "\n";
print $V, " comma\n" and $n += 10;
print "n=$n\n";

open my $fh, '>', \my $buf or die "open: $!";
print $fh "one" or die "print: $!";
print {$fh} "-two" and print $fh "-three";
printf $fh "-%s", "four" or die;
close $fh;
print "buf=[$buf]\n";

unless (0) { print "u" and print "v\n" }
print "w" and print "x" and print "y\n";
print "z" or print "no" or print "no\n";
print "\n";

# ---- reverse as a list-context print argument ----
print reverse(@a), "\n";
print reverse(1 .. 5), "\n";
print "[", reverse("ab", "cd", "ef"), "]\n";
print reverse("xyz"), "\n";
print scalar(reverse("xyz")), "\n";
say reverse @a;
say reverse map { $_ * 2 } @a;
say reverse sort { $a <=> $b } (3, 1, 2);
say join ",", reverse @a;
say reverse %h;
my $s = sprintf "%s%s%s", reverse @a;
say $s;
my $r = reverse "hello";
say $r;
say scalar reverse "hello", "world";
my @empty;
print "empty:[", reverse(@empty), "]\n";
my $aref = [4, 5, 6];
print reverse(@$aref), "\n";
print reverse(@a, 9, @$aref), "\n";
