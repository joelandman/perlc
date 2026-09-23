use experimental qw(signatures);
sub inc($n) { $n + 1 }
sub pair($a, $b = 0) { $a + $b }
print inc(4), "\n";
print pair(10), " ", pair(10, 2), "\n";
print "done\n";
