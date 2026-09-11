#!/usr/bin/perl
# Smoke test for D131: `our $var;` declared inside a nested bare
# `{ }` block (not true file/package scope) — a sub referencing it
# from outside the block used to see nothing.
use strict;
use warnings;

{
    our $result;
    sub foo { local($_) = shift; $result = $_; }
    foo("hi");
    print "$result\n";
}
