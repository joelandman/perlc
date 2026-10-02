mkdir "/tmp/perlc_dgi_smoke";
open(W, ">", "/tmp/perlc_dgi_smoke/a.tmp") or die $!;
close W;
my $dir = "/tmp/perlc_dgi_smoke";
my @f = <$dir/*.tmp>;
print scalar(@f), "\n";
unlink "/tmp/perlc_dgi_smoke/a.tmp";
rmdir "/tmp/perlc_dgi_smoke";
