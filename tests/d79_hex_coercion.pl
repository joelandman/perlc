#!/usr/bin/perl
# Deep: D79 — decimal-only string coercion (hex/oct/bin only via hex()/oct())
print "=== add ===\n";
print "0x10+1=", ("0x10"+1), "\n";
print "0xFF+0=", ("0xFF"+0), "\n";
print "0X10+0=", ("0X10"+0), "\n";
print "0b10+0=", ("0b10"+0), "\n";
print "010+1=", ("010"+1), "\n";
print "12abc+0=", ("12abc"+0), "\n";
print " 0x10+0=", (" 0x10"+0), "\n";

print "=== other ops ===\n";
print "mul=", ("0x10" * 1), "\n";
print "int=", int("0x10"), "\n";
print "abs=", abs("0x10"), "\n";
print "cmp16=", ("0x10" == 16 ? "y" : "n"), "\n";
print "cmp0=", ("0x10" == 0 ? "y" : "n"), "\n";

print "=== explicit ===\n";
print "hex=", hex("0x10"), "\n";
print "oct=", oct("010"), "\n";

print "d79_hex_coercion_done\n";
