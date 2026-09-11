#!/usr/bin/perl
# Smoke test for D115: bare `return;` in list context yielded a
# 1-element (undef) list instead of Perl's empty list.
use strict;
use warnings;

sub f { return; }
my @r = f();
print scalar(@r), "\n";
