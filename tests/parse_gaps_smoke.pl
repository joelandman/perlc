my $obj = bless {}, "C";
sub C::m { return "dyn" }
my $m = "m";
my $r = $obj->$m();
print "r=$r\n";
print $obj->$m(), "|", "x", "\n";
my @out = map { { k => $_ } } (1..3);
print scalar(@out), ":", $out[0]{k}, $out[2]{k}, "\n";
my @r2 = grep { defined } (1, undef, 2);
print "grep=", scalar(@r2), "\n";
my $i = 0;
while ($i < 3) { $i++; } continue { }
print "whilecont-ok\n";
print "smoke_done\n";
