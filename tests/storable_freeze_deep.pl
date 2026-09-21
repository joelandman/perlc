use Storable qw(freeze thaw nfreeze store retrieve nstore dclone);
my $tmp = "storable_freeze_deep.tmp";

my $h = {a=>1, b=>[2,3], c=>{d=>"hi"}, u=>undef, n=>-7, f=>1.5};
my $y = thaw(freeze($h));
print "round=$y->{a} $y->{b}[1] $y->{c}{d} u=", (defined $y->{u} ? 1 : 0), " n=$y->{n}\n";
print "same_ref=", ($y == $h ? 1 : 0), "\n";

my %cyc; $cyc{self} = \%cyc; $cyc{v} = 3;
my $c = thaw(freeze(\%cyc));
print "cyc=$c->{v} self_ok=", ($c->{self} == $c ? 1 : 0), "\n";

my $sref = thaw(freeze(\(my $x = 11)));
print "sref=$$sref\n";

my $blessed = bless {k=>4}, "B";
my $bb = thaw(freeze($blessed));
print "bless=", ref($bb), " k=$bb->{k}\n";

store($h, $tmp);
my $r = retrieve($tmp);
print "store=$r->{a} $r->{c}{d}\n";
nstore($h, $tmp);
my $r2 = retrieve($tmp);
print "nstore=$r2->{b}[0]\n";
unlink $tmp;

my $n = thaw(nfreeze([1,2,3]));
print "narr=@$n\n";
sub hx { join("", map { sprintf("%02x", ord($_)) } split(//, $_[0])) }
print "ihex=", hx(nfreeze(\42)), "\n";
print "shex=", hx(nfreeze(\"hi")), "\n";
print "done\n";
