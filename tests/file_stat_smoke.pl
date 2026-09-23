use File::stat;
my $p = "/tmp/perlc_fstat_smoke.txt";
open my $fh, ">", $p; print $fh "abcdef"; close $fh;
my $s = stat($p);
print "isa=", ref($s), "\n";
print "size=", $s->size, "\n";
unlink $p;
print "done\n";
