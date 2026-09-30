#!/usr/bin/perl
# Deep test for File::Which: which()/where(), including real File::Which's
# context-sensitivity — which() itself is genuinely context-sensitive
# (scalar: first PATH match, or undef; list: EVERY match, identical to
# where()); where() in *scalar* context returns the match COUNT, not a
# path. Both nuances verified against the real installed module before
# implementing, not guessed. A name containing '/' is checked directly
# (no PATH search).
#
# Uses a private, fully controlled scratch PATH (three directories, two
# of them containing an executable of the same name, one a
# non-executable same-named file that must be skipped) rather than
# relying on whatever happens to be installed on the host, so results
# are deterministic everywhere.
use strict;
use warnings;
use File::Which qw(which where);

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $base = "/tmp/perlc_fwhich_deep";
system("rm -rf $base");
mkdir $base;
mkdir "$base/d1";
mkdir "$base/d2";
mkdir "$base/d3";

# d1: a non-executable file named "tool" (must be skipped)
open(my $fh1, '>', "$base/d1/tool") or die $!;
print $fh1 "not executable\n";
close $fh1;
chmod 0644, "$base/d1/tool";

# d2 and d3: real executables named "tool"
for my $d (qw(d2 d3)) {
    open(my $fh, '>', "$base/$d/tool") or die $!;
    print $fh "#!/bin/sh\necho hi\n";
    close $fh;
    chmod 0755, "$base/$d/tool";
}

my $orig_path = $ENV{PATH};
local $ENV{PATH} = "$base/d1:$base/d2:$base/d3";

# which() scalar context: first EXECUTABLE match, skipping d1's
# non-executable same-named file
my $first = which("tool");
check('which_scalar_skips_nonexec', $first eq "$base/d2/tool");

# which() list context: every match (not just one)
my @all_which = which("tool");
check('which_list_all_matches', join(",", @all_which) eq "$base/d2/tool,$base/d3/tool");

# where() list context: identical to which() in list context
my @all_where = where("tool");
check('where_list_all_matches', join(",", @all_where) eq "$base/d2/tool,$base/d3/tool");

# where() scalar context: the COUNT, not a path
my $count = where("tool");
check('where_scalar_is_count', $count == 2);

# no match anywhere in PATH
check('no_match_scalar_undef', !defined(which("nope_xyz_tool")));
my @none = which("nope_xyz_tool");
check('no_match_list_empty', scalar(@none) == 0);
my $none_count = where("nope_xyz_tool");
check('no_match_where_count_zero', $none_count == 0);

# a name containing '/' is checked directly, no PATH search
check('direct_path_executable', which("$base/d3/tool") eq "$base/d3/tool");
check('direct_path_nonexecutable', !defined(which("$base/d1/tool")));

$ENV{PATH} = $orig_path;
system("rm -rf $base");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "file_which_done\n";
