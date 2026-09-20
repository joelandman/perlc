use File::Path qw(make_path remove_tree mkpath rmtree);
system("rm -rf /tmp/perlc_fpath_deep");
# old-style positional API
my @r = mkpath("/tmp/perlc_fpath_deep/p", 1);
print "r=[@r]\n";
# old-style arrayref + verbose
system("rm -rf /tmp/perlc_fpath_deep");
my @r2 = mkpath(["/tmp/perlc_fpath_deep/x/y", "/tmp/perlc_fpath_deep/z"], 0);
print "r2=[@r2]\n";
print "xy=", (-d "/tmp/perlc_fpath_deep/x/y" ? 1 : 0),
      " z=", (-d "/tmp/perlc_fpath_deep/z" ? 1 : 0), "\n";
# scalar context = count
system("rm -rf /tmp/perlc_fpath_deep");
my $n = make_path("/tmp/perlc_fpath_deep/a/b/c");
print "n=$n\n";
# new-style opts hash: verbose output
make_path("/tmp/perlc_fpath_deep/d/e", {verbose => 1});
# remove_tree with a file inside; verbose output shape
open(my $fh, ">", "/tmp/perlc_fpath_deep/d/e/f.txt") or die;
print $fh "data\n";
close $fh;
my @r3 = remove_tree("/tmp/perlc_fpath_deep", {verbose => 1});
print "r3=[@r3]\n";
print "gone=", (-e "/tmp/perlc_fpath_deep" ? 1 : 0), "\n";
# remove_tree list context returns per-dir counts
make_path("/tmp/perlc_fpath_deep/q/r", "/tmp/perlc_fpath_deep/s");
open($fh, ">", "/tmp/perlc_fpath_deep/q/r/file2.txt") or die; close $fh;
my @r4 = remove_tree("/tmp/perlc_fpath_deep/q", "/tmp/perlc_fpath_deep/s");
print "r4=[@r4]\n";
# scalar context on remove_tree = total
make_path("/tmp/perlc_fpath_deep/t");
my $n2 = remove_tree("/tmp/perlc_fpath_deep");
print "n2=$n2\n";
print "done\n";
