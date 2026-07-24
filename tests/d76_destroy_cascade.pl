#!/usr/bin/perl
# Deep: D76 — nested DESTROY cascade for hash/array-backed objects.
# Print-based (order matches real Perl); avoids package-array sharing gaps.
use strict;

package Inner;  sub DESTROY { print "I\n" }
package Outer;  sub DESTROY { print "O\n" }
package Middle; sub DESTROY { print "M\n" }
package main;

print "=== hash_nested ===\n";
{ my $o = bless { i => bless({}, "Inner") }, "Outer"; }

print "=== array_nested ===\n";
{ my $o = bless [ bless({}, "Inner") ], "Outer"; }

print "=== hash_holds_array_obj ===\n";
{ my $o = bless { i => bless([], "Inner") }, "Outer"; }

print "=== shared_named_inner ===\n";
{
    my $inner = bless({}, "Inner");
    my $o = bless { i => $inner }, "Outer";
}

print "=== three_level ===\n";
{
    my $o = bless {
        m => bless { i => bless({}, "Inner") }, "Middle"
    }, "Outer";
}

print "=== standalone_inner ===\n";
{ my $i = bless({}, "Inner"); }

print "=== standalone_outer ===\n";
{ my $o = bless {}, "Outer"; }

print "d76_destroy_cascade_done\n";
