#!/usr/bin/perl
# Smoke test: a bare `;` null statement right after a block-ending
# statement (`if (...) { ... };`) is legal in real Perl but was a hard
# parser error ("unexpected token ';'") in perlc.
use strict;
use warnings;

sub f {
    my $x = shift;
    if ($x =~ /a/) { return "A" };
    return "none";
}

print f("a"), "\n";
print f("z"), "\n";
