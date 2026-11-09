#!/usr/bin/perl
# D239: integer version, a sub after the version clause, and a package
# with no version leaves $VERSION unset. Interpolation of the qualified
# name sees the same scalar.
use strict;
use warnings;
package Foo v6.0.2;
sub hi { return "hi" }
package Num 5;
package Plain;
sub id { return "plain" }
package main;
print Foo::hi(), " $Foo::VERSION\n";
print "Num=$Num::VERSION\n";
print Plain::id(), " ", (defined($Plain::VERSION) ? $Plain::VERSION : "U"), "\n";
print "ge=", ($Foo::VERSION ge "v6") ? "Y" : "N", "\n";
