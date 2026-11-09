#!/usr/bin/perl
# D239: package NAME VERSION sets the package $VERSION scalar.
use strict;
use warnings;
package Foo v6.0.2;
package Bar 1.23;
package Qux v1.2.3;
package main;
print "Foo=$Foo::VERSION\n";
print "Bar=$Bar::VERSION\n";
print "Qux=$Qux::VERSION\n";
print "eq=", ($Foo::VERSION eq "v6.0.2") ? "Y" : "N", "\n";
print "bare=", defined($main::VERSION) ? $main::VERSION : "U", "\n";
