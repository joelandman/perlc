use Storable qw(dclone);
my $h = {a=>1, b=>[1,2], c=>{d=>"x"}};
my $c = dclone($h);
print "ref_h=", ref($c), " ref_b=", ref($c->{b}), " ref_c=", ref($c->{c}), "\n";
print "vals=$c->{a} $c->{b}[1] $c->{c}{d}\n";
$c->{a}=99; $c->{b}[1]=88; $c->{c}{d}="y";
print "orig=$h->{a} $h->{b}[1] $h->{c}{d}\n";
print "clone=$c->{a} $c->{b}[1] $c->{c}{d}\n";
print "done\n";
