use lib 'tests/lib';
use E::Tagged qw(:all);
print "m: ", munge("a"), "\n";
print "f: ", frobnicate(1,2), "\n";
print "c: ", $E::Tagged::Cfg, "\n";
my %h = (all => [1,2], basic => ['m']);
print "hash: ", scalar(keys %h), "\n";
print "interp: munge=$h{all}[0]\n";
print "deep_done\n";
