my %h = (all=>1, basic=>2);
my @s = @h{qw(all basic)};
print "s: @s\n";
my $r = {all => [1,2], basic => ['m']};
my @a = @{$r->{all}};
print "a: @a\n";
print "done\n";
