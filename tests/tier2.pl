use feature "say";
#!/usr/bin/perl
use strict;
use warnings;

# ── $. (line number) ──────────────────────────────────────────────────────────
my $tmpfile = "/tmp/tier2_test_$$.txt";
open(my $fh, '>', $tmpfile) or die "open: $!";
print $fh "line1\n";
print $fh "line2\n";
print $fh "line3\n";
close($fh);

open(my $fh2, '<', $tmpfile) or die "open: $!";
while (<$fh2>) {
    chomp;
}
close($fh2);
print "dot=$.\n";  # should be 3

# ── $, and $\ ────────────────────────────────────────────────────────────────
{
    local $, = "-";
    local $\ = "\n";
    print "a", "b", "c";   # a-b-c\n
}

# ── $& (last match string) ───────────────────────────────────────────────────
my $str = "hello world";
$str =~ /wo(r)ld/;
print "amp=$&\n";   # world

# ── POSIX functions ──────────────────────────────────────────────────────────
use POSIX qw(floor ceil fmod strftime);
print "floor=", POSIX::floor(3.7), "\n";   # 3
print "ceil=",  POSIX::ceil(3.2),  "\n";   # 4
print "fmod=",  POSIX::fmod(10.0, 3.0), "\n";  # 1

# ── Scalar::Util ─────────────────────────────────────────────────────────────
use Scalar::Util qw(blessed reftype looks_like_number);
my $obj = bless {}, "MyClass";
print "blessed=", Scalar::Util::blessed($obj), "\n";   # MyClass
print "reftype=", Scalar::Util::reftype($obj), "\n";   # HASH
print "lln1=",    Scalar::Util::looks_like_number(42), "\n";  # 1
print "lln2=",    Scalar::Util::looks_like_number("foo"), "\n"; # 0
print "lln3=",    Scalar::Util::looks_like_number("3.14"), "\n"; # 1

# ── seek / tell / binmode ────────────────────────────────────────────────────
open(my $sfh, '<', $tmpfile) or die "open: $!";
binmode($sfh);
my $line1 = <$sfh>;
my $pos   = tell($sfh);
seek($sfh, 0, 0);  # rewind
my $again = <$sfh>;
close($sfh);
chomp $line1; chomp $again;
print "seek_ok=", ($line1 eq $again ? "yes" : "no"), "\n";   # yes
print "tell_ok=", ($pos > 0 ? "yes" : "no"), "\n";           # yes

# ── stat ─────────────────────────────────────────────────────────────────────
my @st = stat($tmpfile);
print "stat_ok=", (scalar(@st) == 13 ? "yes" : "no"), "\n";  # yes
print "size_ok=", ($st[7] > 0 ? "yes" : "no"), "\n";         # yes

# ── glob ─────────────────────────────────────────────────────────────────────
my @files = glob("/tmp/tier2_test_*.txt");
print "glob_ok=", (scalar(@files) >= 1 ? "yes" : "no"), "\n";  # yes

# ── isa / can ────────────────────────────────────────────────────────────────
package Animal;
sub new { bless {}, shift }
sub speak { "..." }

package Dog;
our @ISA = ('Animal');
sub new { bless {}, shift }
sub speak { "woof" }

package main;
my $dog = Dog->new();
print "isa=",  ($dog->isa("Animal") ? "yes" : "no"), "\n";   # yes
print "isa2=", ($dog->isa("Dog")    ? "yes" : "no"), "\n";   # yes
print "isa3=", ($dog->isa("Cat")    ? "no"  : "no"), "\n";   # no
my $canspeak = $dog->can("speak");
print "can=",  (defined($canspeak) ? "yes" : "no"), "\n";    # yes

# cleanup
unlink $tmpfile;
print "done\n";
