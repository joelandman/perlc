# Deep test for D210: `\my %hash` / `\my @arr` — refs to freshly-declared
# inline lexicals.
#
# Root cause: CodeGen::emitExpr's NK::RefScalar case did
#   perl_ref_scalar(emitExpr(*n.left))
# for EVERY ref, including one whose operand is an NK::My node (the inline
# declaration of \my %opt). My's own emitExpr returned perl_hash_size (the
# empty hash's element count = 0) for the '%'/'@' sigils, so perl_ref_scalar
# wrapped that number: \my %opt became SCALAR(0x...) instead of HASH(0x...),
# and \my @opt became SCALAR instead of ARRAY. A sub expecting a real hash
# (GetOptions(\my %opt, ...)) therefore received a stray scalar.
#
# Fix: when the ref's operand is an NK::My with a '%' or '@' sigil, emit the
# declaration (which creates the fresh empty container in the current scope)
# and return perl_ref_hash / perl_ref_array of that container. \my $scalar
# keeps the old scalar-ref path.
#
# NB: this deliberately avoids `my (%h) = @_` unpacking (a separate,
# pre-existing nondeterministic sub-arg bug unrelated to D210); the cases
# below use direct list-assignment and variable captures instead.

use strict;
use warnings;

# --- \my %hash ---
my $rh = \my %opt;
$rh->{name} = "widget";
$rh->{qty} = 7;
print "hash.ref=", ref($rh), "\n";
print "hash.count=", scalar(keys %$rh), "\n";
print "hash.name=", $rh->{name}, "\n";

# ref captured inside a list element keeps its type
my @cells = (\my %bucket);
print "bucket.ref=", ref($cells[0]), "\n";
$cells[0]{k} = 1;
print "bucket.k=", $cells[0]{k}, "\n";

# a fresh one is empty and independently typed
my $empty = \my %other;
print "empty.ref=", ref($empty), "\n";
print "empty.count=", scalar(keys %$empty), "\n";

# --- \my @arr ---
my $ar = \my @list;
$ar->[0] = "first";
$ar->[1] = "second";
print "arr.ref=", ref($ar), "\n";
print "arr.first=", $ar->[0], "\n";
print "arr.at2=", $ar->[1], "\n";

my @acells = (\my @stack);
$acells[0]->[0] = 99;
print "stack.ref=", ref($acells[0]), "\n";
print "stack.99=", $acells[0]->[0], "\n";

# --- \my %h as the only element of an ArrayLit, read back ---
my @only = (\my %single);
print "single.ref=", ref($only[0]), "\n";

# --- \my %fresh standalone ---
my $fresh = \my %fresh;
$fresh->{z} = 3;
print "fresh.ref=", ref($fresh), " fresh.z=", $fresh->{z}, "\n";
