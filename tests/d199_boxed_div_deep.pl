#!/usr/bin/env perl
# D199 tail deep: the boxed-path zero-divisor dies match real perl exactly —
# $@ compared with eq against the full expected location text, nested evals
# (inner catches first), $@ preserved across two catches, the die's effect on
# the enclosing list/return context, and the uncaught exit-255 finale.
use strict; use warnings;

my @d = (0, 3);
my ($fn, $line) = ($0, 0);

# 1. exact $@ text via eq
eval { my $q = 5 / $d[0]; 1 };
my $at = $@;
$line = 12;
if ($at eq "Illegal division by zero at $0 line 12.\n") {
    print "exact-match: yes\n";
} else {
    print "exact-match: NO [$at]\n";
}

# 2. chomp+split form
my ($msg, $loc) = split / at /, $at, 2;
print "msg=$msg\n";
print "locfile=", (split / line /, $loc)[0], "\n";
print "locline=", (split / line /, $loc)[1];

# 3. nested evals: the inner handler catches first
eval {
    eval { my $q = 5 / $d[0]; 1 } or print "inner-caught: $@";
    print "inner-after\n";
    1
} or print "outer-caught: $@";
print "outer-after\n";

# 4. $@ preserved across a second, successful eval
eval { my $ok = 1 + 1; 1 };
print "at-after-ok: $at";

# 5. die inside a list-context expression (the assignment never happens)
my @out;
eval { @out = (1, 2, 5 / $d[0], 4); 1 } or print "list-die: $@";
print "out=(", join(",", @out), ")\n";

# 6. %= through the boxed path, then the i64 path, locations differ
my $a = 6; my $b = 0;
eval { $a %= $d[0]; 1 } or print "mod1: $@";
eval { $a %= 0; 1 } or print "mod2: $@";
($msg, $loc) = split / at /, $@, 2;
print "modmsg=$msg\n";

# 7. flat-row /= zero divisor, then a successful one after the catch
my @rows = ([1.0, 2.0]);
eval { $rows[0][1] /= $d[0]; 1 } or print "fr: $@";
eval { $rows[0][1] /= 2.0; 1 };
print "row0: $rows[0][1]\n";

# 8. die does not leak into the next statement
my $sum = 0;
for my $k (0 .. 2) {
    eval { my $t = 10 / ($k == 1 ? $d[0] : 1); $sum += $t; 1 } or print "k$k: $@";
}
print "sum=$sum\n";
print "finale-next\n";

# 9. uncaught at top-level: perl dies with location on stderr, rc 255
my $fatal = 5 / $d[0];
print "never printed\n";
