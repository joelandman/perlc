print(-t STDIN ? "tty\n" : "notty\n");
my $i = 0;
do { $i++; } while !($i >= 3);
print "i=$i\n";
