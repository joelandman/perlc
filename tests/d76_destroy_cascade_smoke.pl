#!/usr/bin/perl
# Smoke: D76 — DESTROY must cascade to nested blessed objects.
# Uses print-based logging (package @main::log / our @log sharing is a
# separate gap; DESTROY itself is verified via printed order).
use strict;

package Inner; sub DESTROY { print "Inner\n" }
package Outer; sub DESTROY { print "Outer\n" }
package main;

{
    my $o = bless { i => bless({}, "Inner") }, "Outer";
}
print "d76_destroy_cascade_smoke_done\n";
