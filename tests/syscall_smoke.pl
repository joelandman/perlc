#!/usr/bin/perl

# Smoke test for syscall implementation
use strict;
use warnings;

print "Testing syscall implementation...\n";

# Test basic getpid syscall
my $SYS_getpid = 39;
my $pid = syscall($SYS_getpid);
print "getpid result: $pid (defined: " . (defined $pid ? "yes" : "no") . ")\n";
exit(1) unless defined $pid;
exit(1) unless $pid > 0;

# Test basic getuid syscall  
my $SYS_getuid = 102;
my $uid = syscall($SYS_getuid);
print "getuid result: $uid (defined: " . (defined $uid ? "yes" : "no") . ")\n";
exit(1) unless defined $uid;
exit(1) unless $uid >= 0;

# Test basic getgid syscall
my $SYS_getgid = 104;
my $gid = syscall($SYS_getgid);
print "getgid result: $gid (defined: " . (defined $gid ? "yes" : "no") . ")\n";
exit(1) unless defined $gid;
exit(1) unless $gid >= 0;

print "All syscall smoke tests passed!\n";