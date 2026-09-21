use Storable qw(freeze thaw dclone nfreeze);
my $h = {a=>1, b=>[2,3], c=>{d=>"x"}};
my $y = thaw(freeze($h));
print "vals=$y->{a} $y->{b}[1] $y->{c}{d}\n";
my $z = thaw(nfreeze($h));
print "nvals=$z->{a} $z->{b}[0]\n";
print "done\n";
