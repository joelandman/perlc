use strict; use warnings;
my $cls = "Pkg::Sub";
eval "sub $cls() { return 7 }";
print "s=", Pkg::Sub(), "\n";
my @names = ("F1", "F2");
foreach my $f (@names) {
    eval "sub $f () { return 5 }";
}
print "f=", F1(), F2(), "\n";
# runtime-defined sub with args called later
eval 'sub dyn_sub { my ($x) = @_; return $x * 3 }';
print "d=", dyn_sub(14), "\n";
# constant-like pattern with interpolated value
my $v = 99;
eval "sub Const99() { return $v }";
print "q=", Const99(), "\n";
print "deep_done\n";
