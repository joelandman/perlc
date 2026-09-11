#!/usr/bin/perl
# Smoke test for D102: die REF / die $blessed_obj lost the reference
# in $@ (stringified to "HASH(0xaddr) at FILE line N." instead of
# staying a real reference).
use strict;
use warnings;

eval { die { code => 42 }; };
print ref($@), " ", $@->{code}, "\n";
