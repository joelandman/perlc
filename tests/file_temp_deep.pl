use File::Temp qw(tempdir tempfile mkstemp mkdtemp mktemp tmpnam);
system("rm -rf /tmp/perlc_ftdeep");
# template with fixed prefix
my $d = tempdir("/tmp/perlc_ftdeep_XXXXXX");
print "d_ok=", ($d =~ m{^/tmp/perlc_ftdeep_[A-Za-z0-9_]{6}$} ? 1 : 0), " dir=", (-d $d ? 1 : 0), "\n";
# write via the tempfile fh, read back
my ($fh, $fn) = tempfile(DIR => $d);
print "fn_ok=", (index($fn, "$d/") == 0 ? 1 : 0), "\n";
print $fh "alpha\nbeta\n"; close $fh;
open(my $in, "<", $fn); local $/; my $txt = <$in>; close $in;
print "txt=[$txt]\n";
# no-arg forms
my ($fh2, $fn2) = tempfile();
(my $fn2dir, my $fn2base) = ($fn2 =~ m{^(.*)/([^/]+)$});
print "fn2_base_ok=", ($fn2base =~ m{^[A-Za-z0-9_]{10}$} ? 1 : 0),
      " fn2_dir_exists=", (-d $fn2dir ? 1 : 0), "\n";
close $fh2; unlink $fn2;
# SUFFIX option
my ($fh3, $fn3) = tempfile(SUFFIX => ".dat");
print "suf_ok=", ($fn3 =~ m/\.dat$/ ? 1 : 0), "\n";
close $fh3; unlink $fn3;
# mkstemp / mkdtemp / mktemp
my ($fh4, $fn4) = mkstemp("/tmp/perlc_ftmks_XXXXXX");
print "mks_ok=", ($fn4 =~ m{^/tmp/perlc_ftmks_[A-Za-z0-9_]{6}$} ? 1 : 0), " exists=", (-e $fn4 ? 1 : 0), "\n";
print $fh4 "zz\n"; close $fh4;
print "mks_size=", (-s $fn4), "\n";
my $dd = mkdtemp("/tmp/perlc_ftmkd_XXXXXX");
print "mkd_ok=", ($dd =~ m{^/tmp/perlc_ftmkd_[A-Za-z0-9_]{6}$} ? 1 : 0), " dir=", (-d $dd ? 1 : 0), "\n";
rmdir $dd;
my $t = mktemp("/tmp/perlc_ftmkt_XXXXXX");
print "mkt_ok=", ($t =~ m{^/tmp/perlc_ftmkt_[A-Za-z0-9_]{6}$} ? 1 : 0), " created=", (-e $t ? 1 : 0), "\n";
# tmpnam shape only (no creation contract)
my $tn = tmpnam();
print "tmpnam_ok=", ($tn =~ m{^/tmp/[A-Za-z0-9_]{10}$} ? 1 : 0), "\n";
# scalar context tempfile returns the fh
my $sc = tempfile();
print "scalar_def=", (defined $sc ? 1 : 0), "\n";
close $sc;
print "done\n";
