use constant D => (32);
my @c = D;
print "const=", scalar(@c), ":$c[0]\n";
print "arith=", D + 1, "\n";

sub g { return (1,2); }
my ($g1, $g2) = g(); print "g=$g1,$g2\n";
print "scalar-g=", scalar(g()), "\n";

sub h { return (0); }
print "zero=", (h() || "fb"), "\n";
my @z = h(); print "z=", scalar(@z), ":$z[0]\n";

sub u { return (undef); }
my @u = u(); print "u=", scalar(@u), ":", (defined($u[0]) ? "def" : "undef"), "\n";

sub imp { (f()); }
my $i = imp(); print "imp=$i\n";

sub nest { if (shift()) { return (7); } return 9; }
print "nest=", nest(1), "\n";

sub paren { return ( (32), (33) ); }
my ($p, $q) = paren(); print "paren=$p,$q\n";

sub mix { return (32, "str"); }
my ($m1, $m2) = mix(); print "mix=$m1,$m2\n";

my %h = (k => f());
print "hash=$h{k}\n";
my @l = (1, f(), 3);
print "list=@l\n";

sub w { return wantarray ? "L" : "S"; }
my $ws = w(); my @wl = w();
print "want=$ws,@wl\n";
# widen: return (32) in scalar/numeric/boolean/map contexts
sub f2 { return (32); }
print f2() + 1, "\n";
if (f2()) { print "truthy\n"; }
my @a2 = map { $_ * 2 } f2();
print "map=@a2\n";
sub g2 { return ((1)); }
my ($x2) = g2(); print "x2=$x2\n";
print "eq=", (f2() == 32 ? "y" : "n"), "\n";
# bare return; stays empty (D115 regression guard)
sub e { return; }
my @e2 = e(); print "bare=", scalar(@e2), "\n";
my $e3 = e(); print "bare-s=", (defined($e3) ? "def" : "undef"), "\n";
# return (1,2) in scalar context = 2 (real Perl: last element)
sub t { return (1,2); }
print "sc=", t(), "\n";
