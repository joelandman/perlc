#!/usr/bin/env perl
# Smoke test: Time::Local — timelocal/timegm via default @EXPORT, a couple
# of fixed UTC dates (deterministic everywhere).
use strict;
use warnings;
use Time::Local;

print timegm(0,0,12,1,0,100), "\n";
print timegm(0,0,0,1,0,70), "\n";
print "smoke_done\n";