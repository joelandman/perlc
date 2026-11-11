# D264: self-require, a second require, a path require, and a require cycle.
use strict;
use warnings;
use lib 'tests/lib';
BEGIN {
    my $old = defined $ENV{PERL5LIB} ? $ENV{PERL5LIB} : '';
    $ENV{PERL5LIB} = ($old eq '') ? 'tests/lib' : "tests/lib:$old";
}
my $m = 'D264Self';
my $ok = eval "require $m;";
my $at = $@;
print "self=", ($ok ? 1 : 0), " at=", ($at eq '' ? "empty" : "set"), "\n";
my $ok2 = eval "require $m;";
print "again=", ($ok2 ? 1 : 0), "\n";
my $path = '"D264Self.pm"';
my $ok3 = eval "require $path;";
print "path=", ($ok3 ? 1 : 0), "\n";
my $a = 'D264A';
my $oka = eval "require $a;";
my $ata = $@;
print "cycle=", ($oka ? 1 : 0), " at=", ($ata eq '' ? "empty" : "set"), "\n";
my $oka2 = eval "require $a;";
print "cycle2=", ($oka2 ? 1 : 0), "\n";
