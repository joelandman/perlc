use strict;
use warnings;
# Encode is in archlib; Carp is in privlib. Both are on the default path.
my $e = eval { require Encode };
my $c = eval { require Carp };
print "e=", defined($e) ? $e : "U", " c=", defined($c) ? $c : "U", "\n";
my $m = eval { require D254::Missing::Deep };
print "miss=", defined($m) ? $m : "U", "\n";
# A -I-style relative name with a slash stays a path and stays missing.
my $p = eval { require "No/Such/D254.pm" };
print "path=", defined($p) ? $p : "U", "\n";
