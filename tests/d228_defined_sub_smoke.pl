# D228: defined &NAME does not call. A glob-installed constant is false
# in `NAME and ...`.
use strict;
use warnings;
sub foo { 7 }
sub zap { return undef }
sub onlyfwd;
print defined &foo ? "fooY\n" : "fooN\n";
print defined &onlyfwd ? "fwdY\n" : "fwdN\n";
print defined &nosuch ? "noY\n" : "noN\n";
print defined(&zap) ? "zapY\n" : "zapN\n";
print defined(&zap()) ? "callY\n" : "callN\n";
print defined &hasbody ? "bodyY\n" : "bodyN\n";
sub hasbody { 1 }

package A;
BEGIN { *DEBUG = sub () { 0 } }
package B;
our $how;
BEGIN {
    if (defined &DEBUG) { $how = "local" }
    elsif (defined &A::DEBUG) { *DEBUG = \&A::DEBUG; $how = "alias" }
    else { *DEBUG = sub () { 0 }; $how = "fresh" }
}
our $side = 0;
DEBUG and do { $side = 1 };
package main;
print "how=$B::how\n";
print "side=$B::side\n";
sub never { 0 }
never or print "ran\n";
