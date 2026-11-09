#!/usr/bin/perl
# D222: parenthesized `use overload(...)` must not swallow the next sub.
use strict;
use warnings;
package OV;
use overload(
  '""' => \&as_str,
  'fallback' => 1,
);
sub as_str { return "S" }
sub after { return "A" }
package main;
print OV::after(), "\n";
print OV::as_str(), "\n";
