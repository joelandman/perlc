use Hash::Util qw(lock_keys unlock_keys legal_keys);
my %h = (a => 1, b => 2);
lock_keys(%h);
eval { $h{c} = 3; };
print "died=", ($@ ? 1 : 0), "\n";
$h{a} = 99;
print "a=$h{a}\n";
print "legal=", join(",", sort(legal_keys(%h))), "\n";
print "done\n";
