#!/usr/bin/perl
# D271: merged stdout/stderr order matches perl when STDOUT is fully
# buffered and STDERR is unbuffered. A partial line stays in the STDOUT
# buffer until exit, so it follows the warn text.
print "line\n";
print STDERR "err\n";
print "partial";
warn "mid\n";
print "tail\n";
