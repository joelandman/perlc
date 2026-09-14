use feature 'current_sub';
my $fact = sub { my $n = shift; $n <= 1 ? 1 : $n * __SUB__->($n - 1) };
print $fact->(5), "\n";
my $fib; $fib = sub { my $n = shift; $n < 2 ? $n : __SUB__->($n-1) + __SUB__->($n-2) };
print $fib->(10), "\n";
print "end\n";
