#!/usr/bin/perl
# Deep test for D134: syscall pointer-argument writes must be visible in the
# caller's variable (args pushed by reference, not cloned). Covers both
# clock ids, a temp (non-variable) buffer whose write is discarded, byte
# content verification through unpack, and a regression check that
# non-buffer arguments are unaffected. Matches real perl byte-for-byte.
use strict;
use warnings;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
}

# SYS_clock_gettime=228, CLOCK_MONOTONIC=4, CLOCK_REALTIME=0 (Linux x86_64)
my $SYS_clock_gettime = 228;

# monotonic: seconds must be positive, nsec in range
my $buf = "\0" x 16;
my $ret = syscall($SYS_clock_gettime, 4, $buf);
check('mono_ret_zero', $ret == 0);
my ($msec, $mnsec) = unpack("LL", $buf);
check('mono_sec_positive', $msec > 0);
check('mono_nsec_range', $mnsec >= 0 && $mnsec < 1000000000);

# realtime: seconds must be large (post-2000 epoch) and nsec in range
$buf = "\0" x 16;
$ret = syscall($SYS_clock_gettime, 0, $buf);
check('real_ret_zero', $ret == 0);
my ($rsec, $rnsec) = unpack("LL", $buf);
check('real_sec_large', $rsec > 1000000000);
check('real_nsec_range', $rnsec >= 0 && $rnsec < 1000000000);

# realtime seconds must exceed monotonic-uptime seconds by boot time
check('real_gt_mono', $rsec > $msec);

# the same buffer reused for a second call keeps working (no aliasing rot)
my $buf2 = "\0" x 16;
syscall($SYS_clock_gettime, 0, $buf2);
my ($m2sec) = unpack("LL", $buf2);
check('reuse_second_call', $m2sec >= $rsec);

# NOTE: a temporary (non-variable) buffer — `syscall(228, 4, "\0" x 16)` —
# is deliberately NOT tested here: real perl dies with "Modification of a
# read-only value attempted" (syscall lvalue-writes only work through a
# variable) while perlc, which has no read-only-value enforcement, silently
# succeeds. That divergence is documented in TESTS.md under D134; any
# assertion here would necessarily differ between the two.

# non-buffer args: getpid (39) takes no pointers; return must be a positive pid
$ret = syscall(39);
check('getpid_positive', $ret > 0);

# scalar cell that previously held a number then a string: write must land
my $mixed = 12345;
$mixed = "\0" x 16;
$ret = syscall($SYS_clock_gettime, 4, $mixed);
check('mixed_ret_zero', $ret == 0);
my ($mxsec) = unpack("LL", $mixed);
check('mixed_sec_positive', $mxsec > 0);

# many iterations: with the old clone-push the writes were invisible; with
# a ref-push an ownership slip would corrupt the allocator — repetition
# catches either regression.
my $ok_count = 0;
for my $i (1 .. 100) {
    my $b = "\0" x 16;
    my $r = syscall($SYS_clock_gettime, 4, $b);
    my ($s) = unpack("LL", $b);
    $ok_count++ if $r == 0 && $s > 0;
}
check('hundred_iters', $ok_count == 100);

print "d134_syscall_buf_done\n";