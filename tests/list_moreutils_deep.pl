use List::MoreUtils qw(firstidx lastidx onlyidx indexes firstval lastval
                       apply after before after_incl before_incl part
                       any all none notall one true false
                       mesh zip natatime uniq minmax
                       singleton duplicates insert_after);

print "firstidx=", firstidx { $_ eq "c" } qw(a b c d), "\n";
print "lastidx=", lastidx { $_ eq "c" } qw(a c b c), "\n";
print "onlyidx=", onlyidx { $_ eq "b" } qw(a b c), "\n";
print "onlyidx_none=", onlyidx { $_ eq "c" } qw(a c c), "\n";
print "indexes=", join(",", indexes { $_ > 2 } (1,2,3,4,1)), "\n";
print "firstval=", firstval { $_ > 2 } (1,2,3,4), "\n";
print "lastval=", lastval { $_ > 2 } (1,2,3,4), "\n";
print "apply=", join(",", apply { $_ * 2 } (1,2,3)), "\n";
print "after=", join(",", after { $_ == 2 } (1,2,3,4)), "\n";
print "after_incl=", join(",", after_incl { $_ == 2 } (1,2,3,4)), "\n";
print "before=", join(",", before { $_ == 3 } (1,2,3,4)), "\n";
print "before_incl=", join(",", before_incl { $_ == 3 } (1,2,3,4)), "\n";

my @parts = part { $_ % 2 } (1,2,3,4);
print "part0=", join(",", @{$parts[0]||[]}), " part1=", join(",", @{$parts[1]||[]}), "\n";

print "any=", (any { $_ > 3 } 1,2,3,4 ? 1 : 0), "\n";
print "all=", (all { $_ > 0 } 1,2,3 ? 1 : 0), "\n";
print "none=", (none { $_ < 0 } 1,2,3 ? 1 : 0), "\n";
print "notall=", (notall { $_ > 2 } 1,2,3 ? 1 : 0), "\n";
print "one=", (List::MoreUtils::one { $_ == 2 } 1,2,3 ? 1 : 0), "\n";
print "true=", true { $_ > 2 } 1,2,3,4, "\n";
print "false=", false { $_ > 2 } 1,2,3,4, "\n";

my @a = (1,2,3); my @b = (10,20,30);
print "mesh=", join(",", mesh(@a,@b)), "\n";
print "zip=", join(",", zip(@a,@b)), "\n";

my $it = natatime 2, (1,2,3,4,5);
my @c1 = $it->();
my @c2 = $it->();
my @c3 = $it->();
print "nata1=@c1 nata2=@c2 nata3=@c3\n";

print "uniq=", join(",", uniq(1,2,2,3,1)), "\n";
my ($mn,$mx) = minmax(5,1,9,3);
print "minmax=$mn $mx\n";
print "singleton=", join(",", singleton(1,2,2,3)), "\n";
print "duplicates=", join(",", duplicates(1,2,2,3,3,4)), "\n";
my @ins = qw(a b c);
insert_after { $_ eq "b" } "X", @ins;
print "insert=", join(",", @ins), "\n";
print "done\n";
