#!/usr/bin/perl
# D222: use overload with and without parentheses, then a real sub body.
use strict;
use warnings;
package OVD;
use overload(
  '""'   => \&OVD::show,
  'bool' => \&OVD::show,
  'fallback' => 1,
);
sub show { return "shown" }
sub tack {
    my $x = ['', {}, "z"];
    $x->[2] .= "!";
    return $x->[2];
}
package main;
use overload '""' => \&main::id, fallback => 1;
sub id { return "id" }
print OVD::show(), "\n";
print OVD::tack(), "\n";
print id(), "\n";
print "done\n";
