# D195: constant in a list, code ref to ::sub, ternary colon still a colon.
use constant SIG => 42;
sub hello { print "hi\n" }
sub LocalHeader { print "LH\n" }
package Signatures;
my @row = (::SIG, [ \&::LocalHeader, "Local File Header", 0 ]);
print $row[0], " ", $row[1][1], " ", ref($row[1][0]), "\n";
::hello();
::LocalHeader();
package main;
print(1 ? "a" : "b", "\n");
print(::SIG, "\n");
