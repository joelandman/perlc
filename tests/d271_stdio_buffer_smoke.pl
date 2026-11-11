#!/usr/bin/perl
# D271: off a terminal, STDOUT stays fully buffered across a newline, so
# a following warn is already in the merged capture before that line.
print "kept\n";
warn "early\n";
