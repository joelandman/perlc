#!/usr/bin/perl
# Smoke: core process builtins (fork/pipe/waitpid/getppid) vs real Perl.
# getuid/getgid are POSIX, not core — not used here.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

check('getppid_positive', getppid() > 0);
check('$$_positive', $$ > 0);

my $r; my $w;
check('pipe_ok', pipe($r, $w));
my $pid = fork();
if (!defined $pid) {
    check('fork_defined', 0);
} elsif ($pid == 0) {
    close($r);
    print $w "hello";
    close($w);
    exit 0;
} else {
    close($w);
    my $got = <$r>;
    close($r);
    waitpid($pid, 0);
    check('fork_defined', 1);
    check('pipe_msg', defined($got) && $got eq "hello");
    check('wait_exit', ($? >> 8) == 0);
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "ipc_process_smoke_done\n";
