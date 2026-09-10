#!/usr/bin/perl
# Smoke test for D116: __PACKAGE__/__FILE__/__LINE__ compile-time
# constants — a hard parse error before this fix.
use strict;
use warnings;

package Foo::Bar;
sub whoami { return __PACKAGE__; }
package main;
print Foo::Bar::whoami(), "\n";
