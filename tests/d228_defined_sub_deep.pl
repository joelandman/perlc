# D228 deep: qualified existence, parenthesized defined &, and a
# constant sub that must not run the right-hand side of `or`.
use strict;
use warnings;
no warnings 'once';

package A;
sub foo { return 3 }
BEGIN { *BAR = sub () { 0 } unless defined &BAR }
package B;
our $seen = defined &foo ? "Bfoo" : "Bnofoo";
our $afoo = defined &A::foo ? "Afoo" : "Anofoo";
our $abar = defined &A::BAR ? "Abar" : "Anobar";
our $ran = 0;
A::BAR() or do { $ran = 1 };
package main;
print "seen=$B::seen\n";
print "afoo=$B::afoo\n";
print "abar=$B::abar\n";
print "ran=$B::ran\n";
print defined &A::foo ? "topY\n" : "topN\n";
print defined(&A::foo()) ? "topcY\n" : "topcN\n";

sub late { 4 }
print defined &late ? "lateY\n" : "lateN\n";
no strict 'subs';
no warnings 'reserved';
my $k = nothere;
print "k=$k\n";
