# Deep test for D180: sort({ BLOCK } LIST) -- the whole call, including
# the comparator block, wrapped in one set of parens -- was a hard
# "expected ) but got '@'" parse error. Real Perl allows this form;
# found via a real podebconf-report-po-shaped script using
# `sort({ version_cmp($a->[0], $b->[0]) * $sign } @versions)`.
my @versions = ([1,"a"],[3,"c"],[2,"b"]);
my $sign = 1;

my @asc = sort({ $a->[0] <=> $b->[0] } @versions);
print "asc=", join(",", map { $_->[1] } @asc), "\n";

my @desc = sort({ $b->[0] <=> $a->[0] } @versions);
print "desc=", join(",", map { $_->[1] } @desc), "\n";

# signed comparator closing over an outer variable -- the exact
# real-world shape (`... * $sign`)
my @signed = sort({ ($a->[0] <=> $b->[0]) * $sign } @versions);
print "signed=", join(",", map { $_->[1] } @signed), "\n";

# plain numbers, no array-of-arrays
my @nums = sort({ $b <=> $a } 5, 1, 3);
print "nums=", join(",", @nums), "\n";

# single comma-separated element inside the parens (no array var)
my @one = sort({ $a <=> $b } 9);
print "one=", join(",", @one), "\n";

# regressions: unparenthesized and plain-parenthesized forms must stay
# unaffected
my @r1 = sort { $a <=> $b } (3, 1, 2);
print "r1=", join(",", @r1), "\n";
my @r2 = sort(3, 1, 2);
print "r2=", join(",", @r2), "\n";
my %h = (b => 2, a => 1);
my @r3 = sort(keys %h);
print "r3=", join(",", @r3), "\n";

print "done\n";
