#!/usr/bin/perl
# Smoke: D36 — bareword call without parentheses
sub greeter { print "hi:$_[0]\n" }
greeter "world";
greeter("paren");
print "ok\n";
