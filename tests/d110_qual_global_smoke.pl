#!/usr/bin/perl
# Smoke test for D110: a fully-qualified package variable ($Other::x,
# @main::arr, %Other::h, $Data::Dumper::Sortkeys) is a TRUE cross-scope
# global. Setting it at file scope must be visible inside an unrelated
# sub (and vice versa), and element/whole-container access must reach
# the same storage from any scope. All output byte-for-byte vs real perl.
use strict;
use warnings;

# 1. qualified scalar set at file scope, read inside a sub
$Data::Dumper::Sortkeys = 1;
sub f { return $Data::Dumper::Sortkeys; }
print "sub sees: ", f() ? 1 : 0, "\n";

# 2. whole-array copy in a sub sees file-scope writes
@main::arr = (1, 2, 3);
sub g { my @c = @main::arr; return scalar(@c); }
print "arr in sub: ", g(), "\n";

# 3. hash element set at file scope, read in a sub AND at file scope
%Other::h = (k => 5);
sub h2 { return $Other::h{k}; }
print "hash sub: ", (h2() // 'UNDEF'), "\n";
print "hash elem main: ", ($Other::h{k} // 'UNDEF'), "\n";

# 4. write through a sub, read back at file scope and from package Other
$Other::h{k} = 7;
print "hash sub after write: ", (h2() // 'UNDEF'), "\n";
package Other;
sub hpkg { return $Other::h{k} // 'UNDEF'; }
package main;
print "hash from pkg: ", Other::hpkg(), "\n";

# 5. write from a sub, read at file scope
sub w { $Other::x = 11; return $Other::x; }
print "w: ", w(), " back: $Other::x\n";

# 6. qualified scalar read directly in an interpolated string
print "interp: $Other::x\n";

print "d110_smoke_done\n";