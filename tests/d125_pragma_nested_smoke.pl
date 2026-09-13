#!/usr/bin/perl
# Smoke test for D125: use/no pragma statements inside nested scopes
use strict;
use warnings;

sub foo {
    no warnings 'numeric';
    my $x = "abc" + 1;
    return $x;
}

sub bar {
    use strict;
    use warnings;
    return "bar-ok";
}

# bare block
my $r = do {
    no warnings 'uninitialized';
    "block-ok";
};

print foo(), "\n";
print bar(), "\n";
print $r, "\n";
