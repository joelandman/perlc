#!/usr/bin/perl
# D233: parenthesized push of a fat-comma pair into a deref'd row,
# and unshift of a single flag, matching Pod::Text's option loop.
use strict;
use warnings;
my $self = { PENDING => [], MARGIN => 4, INDENTS => [] };
my $attrs = { code => 1 };
push (@{ $$self{PENDING} }, [ $attrs, '' ]);
push (@{ $$self{INDENTS} }, $$self{MARGIN});
my @args;
my $flag = '-a';
if ($flag eq '-a') { push (@args, alt => 1) }
elsif ($flag =~ /^-(\d+)$/) { push (@args, width => $1) }
else { unshift (@_, $flag) }
$flag = '-72';
if ($flag eq '-a') { push (@args, alt => 1) }
elsif ($flag =~ /^-(\d+)$/) { push (@args, width => $1) }
else { unshift (@_, $flag) }
$flag = '-x';
my @rest = ("keep");
if ($flag eq '-a') { push (@args, alt => 1) }
elsif ($flag =~ /^-(\d+)$/) { push (@args, width => $1) }
else { unshift (@rest, $flag) }
print "pending=", scalar(@{ $$self{PENDING} }), "\n";
print "margin=", ${ $$self{INDENTS} }[0], "\n";
print "args=", join(":", @args), "\n";
print "rest=", join(":", @rest), "\n";
my $n = unshift @rest, head => "H";
print "n=$n rest=", join(":", @rest), "\n";
