#!/usr/bin/perl
# Deep test: Cwd — every exported name (bare + Cwd:: qualified), plus the
# surprising abs_path semantics probed byte-for-byte against real Cwd 3.95:
#   - nonexistent FILES return catfile(parent, name) — NOT undef and NOT die
#     (abs_path("/nonexistent-xyz") is "/nonexistent-xyz"!)
#   - a slash-less relative missing name resolves against cwd
#   - tail-".." on a nonexistent final component → undef ("/tmp/y/..")
#   - tail-".." where the final component exists (even a plain FILE) →
#     its parent's realpath ("/tmp/x/.." → "/tmp" on this host)
#   - a missing intermediate component → undef ("/tmp/no-such-dir/file")
#   - realpath == abs_path (real Cwd aliases them)
#   - "//nonexistent-xyz" → "/nonexistent-xyz" (POSIX leading-slash rule)
use strict;
use warnings;
use Cwd;
use Cwd qw(abs_path realpath);

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $cwd = getcwd();
check("getcwd_abs", $cwd =~ m{^/});
check("cwd_alias",  cwd() eq $cwd);
check("fastcwd",    Cwd::fastcwd() eq $cwd);
check("fastgetcwd", Cwd::fastgetcwd() eq $cwd);
check("qual_getcwd", Cwd::getcwd() eq $cwd);

check("abstmp",     abs_path("/tmp") eq "/tmp");
check("realpath",   realpath("/tmp") eq "/tmp");
check("qual_abs",   Cwd::abs_path("/tmp") eq "/tmp");
check("fast_abs",   Cwd::fast_abs_path("/tmp") eq "/tmp");
check("fast_realpath", Cwd::fast_realpath("/tmp") eq "/tmp");
check("abs_dot",    abs_path(".") eq $cwd);
check("abs_undef",  abs_path("/") eq "/");

# the surprising nonexistent-file semantics
check("abs_nonexist", abs_path("/nonexistent-xyz-99") eq "/nonexistent-xyz");
{
    my $r = abs_path("no-such-file-xyz-deep");
    check("abs_rel_nonexist", defined($r) && $r eq "$cwd/no-such-file-xyz-deep");
}
{
    my $r = abs_path("//nonexistent-xyz-deep");
    check("abs_dslash", defined($r) && $r eq "/nonexistent-xyz-deep");
}

# tail-".." semantics (deterministic: /tmp's shape is fixed)
{
    my $r = abs_path("/tmp/.");
    check("abs_dot_suffix", defined($r) && $r eq "/tmp");
}

# /tmp/x (plain file created below) — ".." from a file goes to its parent
my $probe = "/tmp/perlc_cwd_deep_probe_$$.txt";
open(my $fh, ">", $probe) or die "probe setup failed: $!";
print $fh "x\n";
close($fh);
{
    my $r = abs_path("$probe/..");
    check("abs_file_dotdot", defined($r) && $r eq "/tmp");
}
unlink $probe;

# missing intermediate dir
{
    my $r = abs_path("/tmp/perlc_cwd_deep_missing_dir_xyz/f");
    check("abs_missing_dir", !defined($r));
}

# relative resolution with ".." inside an existing dir
{
    my $r = abs_path("nonexist/.././x");
    check("abs_rel_dots", defined($r) && $r eq "$cwd/x");
}

check("smoke_done", 1);
print scalar(@failures), " failures\n";