#!/usr/bin/perl
# Smoke: D79 — implicit string→number must NOT parse 0x as hex
print "a=", ("0x10" + 1), "\n";
print "b=", ("10" + 1), "\n";
print "c=", hex("0x10"), "\n";
print "ok\n";
