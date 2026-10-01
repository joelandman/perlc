mkdir "/tmp/perlc_dglob_smoke";
open(W, ">", "/tmp/perlc_dglob_smoke/a.tmp") or die $!;
close W;
my @f = </tmp/perlc_dglob_smoke/*.tmp>;
print scalar(@f), "\n";
unlink "/tmp/perlc_dglob_smoke/a.tmp";
rmdir "/tmp/perlc_dglob_smoke";
