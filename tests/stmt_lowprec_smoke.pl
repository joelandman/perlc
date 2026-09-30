# D150 smoke: `STMT or/and RHS` for non-print statements (local, state,
# push, unshift, warn, return, next) — RHS used to be silently discarded.
use feature 'state';
our $g = 1; our @ga; our %gh;
sub f { print "f\n"; 0 }
sub t { print "t\n"; 1 }
sub L1 { local $g = 0 or print "local-or\n"; print "g=$g\n" }
L1();
sub L2 { local $g = 5 and print "local-and\n"; print "g=$g\n" }
L2();
sub L3 { local @ga = () or print "local-arr-or\n"; }
L3();
sub L4 { local %gh = (a=>1) and print "local-hash-and\n"; }
L4();
sub L5 { local $_ = "" or print "local-us-or\n"; }
L5();
sub S1 { state $s = 0 or print "state-or\n"; $s++ } S1(); S1();
my @a;
push @a, 1 and print "push-and\n";
push @a, 2 or print "push-or-never\n";
unshift @a, 0 and print "unshift-and\n";
print "a=@a\n";
push(@a, 3) and print "push-paren-and\n";
warn "" and print "warn-and\n";
for (1..2) { next or print "next-or\n"; }
sub R { return f() or print "ret-or-never\n"; } R();
print "done\n";
