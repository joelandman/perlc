use List::MoreUtils qw(firstidx indexes mesh minmax);
print "fi=", firstidx { $_ > 2 } (1,2,3,4), "\n";
print "ix=", join(",", indexes { $_ % 2 } (1,2,3,4)), "\n";
my @a = (1,2); my @b = (3,4);
print "mesh=", join(",", mesh(@a, @b)), "\n";
my ($mn,$mx) = minmax(3,1,4,2);
print "mm=$mn $mx\n";
print "done\n";
