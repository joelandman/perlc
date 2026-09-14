my $aref = [10,20,30];
print "first: $$aref[0]\n";
my %h = (k => 'vv');
my $href = \%h;
print "hash: $$href{k}\n";
my $r = [1,2,3];
print "slice: @{$r}[0,1]\n";
print "arrow: $aref->[1]\n";
print "done\n";
