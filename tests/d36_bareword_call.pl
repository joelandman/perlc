#!/usr/bin/perl
# Deep: D36 — bareword calls without ()
sub greeter { print "hi:$_[0]\n" }
sub multi { print "m=", join(",", @_), "\n" }
sub add1 { return $_[0] + 1 }

print "=== basic ===\n";
greeter "world";
greeter("paren");

print "=== multi arg ===\n";
multi 1, 2, 3;

print "=== in expression ===\n";
my $n = add1 10;
print "n=$n\n";

print "=== bareword string still works ===\n";
my %h = (foo => 1, bar => 2);
print "h=", $h{foo}, $h{bar}, "\n";

print "d36_bareword_call_done\n";
