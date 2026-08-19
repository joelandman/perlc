#!/usr/bin/perl

# In-depth test for syscall implementation
use strict;
use warnings;

print "Testing syscall implementation in depth...\n";

# Test getpid
my $SYS_getpid = 39;
my $pid = syscall($SYS_getpid);
print "getpid: $pid (type: " . ref($pid) . ")\n";
exit(1) unless defined $pid;
exit(1) unless $pid > 0;

# Test getuid
my $SYS_getuid = 102;
my $uid = syscall($SYS_getuid);
print "getuid: $uid (type: " . ref($uid) . ")\n";
exit(1) unless defined $uid;
exit(1) unless $uid >= 0;

# Test getgid
my $SYS_getgid = 104;
my $gid = syscall($SYS_getgid);
print "getgid: $gid (type: " . ref($gid) . ")\n";
exit(1) unless defined $gid;
exit(1) unless $gid >= 0;

# Test error case - invalid syscall number
my $invalid_syscall = 99999;
my $error_result = syscall($invalid_syscall);
print "Invalid syscall result: $error_result (should be -1 or undef)\n";

# Test with arguments - clock_gettime (Linux specific)
my $SYS_clock_gettime = 228;
my $CLOCK_MONOTONIC = 4;
my $buf = "\0" x 16;  # struct timespec: two 8-byte longs
my $ret = syscall($SYS_clock_gettime, $CLOCK_MONOTONIC, $buf);
print "clock_gettime result: $ret (should be 0)\n";
exit(1) unless $ret == 0;

print "All syscall in-depth tests passed!\n";