use Storable;
# nested + shared references preserved by identity (== compares ref addresses)
my $shared = [1,2];
my $h = {x=>$shared, y=>$shared, z=>{k=>$shared}};
print "pre_shared=", ($h->{x} == $h->{y} ? 1 : 0), "\n";
my $c = Storable::dclone($h);
print "shared_same=", ($c->{x} == $c->{y} ? 1 : 0), "\n";
print "shared_neq_orig=", ($c->{x} == $shared ? 1 : 0), "\n";
print "zk_shared=", ($c->{x} == $c->{z}{k} ? 1 : 0), "\n";
print "zk_val=", $c->{z}{k}[1], "\n";
# cycle
my $self = {name=>"root"};
$self->{me} = $self;
my $c2 = Storable::dclone($self);
print "cycle_ref=", (ref($c2->{me}) eq "HASH" ? 1 : 0), "\n";
print "cycle_self=", ($c2->{me} == $c2 ? 1 : 0), "\n";
print "cycle_name=", $c2->{me}{name}, "\n";
# scalar ref
my $s = "hello";
my $sr = \$s;
my $c3 = Storable::dclone($sr);
print "scalar_ref=", (ref($c3) eq "SCALAR" ? 1 : 0), " val=", $$c3, "\n";
$$c3 = "changed";
print "orig_unchanged=", ($s eq "hello" ? 1 : 0), "\n";
# blessed objects
{
    package Point;
    sub new { my ($class, $x, $y) = @_; bless {x=>$x, y=>$y}, $class; }
}
my $p = Point->new(3, 4);
my $c4 = Storable::dclone($p);
print "blessed=", ref($c4), " x=", $c4->{x}, " y=", $c4->{y}, "\n";
$c4->{x} = 30;
print "orig_x=", $p->{x}, " clone_x=", $c4->{x}, "\n";
# deep nesting independence
my $deep = {l=>{l=>{l=>{l=>[1,[2,[3]]]}}}};
my $c5 = Storable::dclone($deep);
$c5->{l}{l}{l}{l}[1][0] = 22;
print "deep_orig=", $deep->{l}{l}{l}{l}[1][0], " deep_clone=", $c5->{l}{l}{l}{l}[1][0], "\n";
print "done\n";
