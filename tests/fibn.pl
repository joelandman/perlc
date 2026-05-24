#!/usr/bin/perl
use strict;
use warnings;

sub fib {
    my ($n) = @_;
    if ( $n <= 1 ) {
        return $n;
    }
    return fib( $n - 1 ) + fib( $n - 2 );
}

my $i = 0;
my $N = shift @ARGV;
foreach $i ( 0 .. $N ) {
    printf "%i\t%li\n", $i, fib($i);
    $i++;    #note the $i here is local to the loop and not the loop
             # counter! this is as per perl's implementation.
}
