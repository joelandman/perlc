use File::Glob qw(:glob);
mkdir "/tmp/perlc_fglob_smoke";
open(W, ">", "/tmp/perlc_fglob_smoke/a.tmp") or die $!;
close W;
my @f = bsd_glob("/tmp/perlc_fglob_smoke/*.tmp");
print scalar(@f), "\n";
print GLOB_TILDE(), "\n";
unlink "/tmp/perlc_fglob_smoke/a.tmp";
