use feature "say";
#!/usr/bin/perl
use strict;
use warnings;

sub fib {
    my ($n) = @_;
    if ($n <= 1) {
        return $n;
    }
    return fib($n - 1) + fib($n - 2);
}

my $i = 0;
while ($i < 10) {
    say fib($i);
    $i++;
}
