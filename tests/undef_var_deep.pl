my $s = "abc";
undef $s;
print "scalar_def=", (defined $s ? 1 : 0), "\n";
$s = "xyz";
undef($s);
print "paren_def=", (defined $s ? 1 : 0), "\n";

my @a = (1,2,3);
undef @a;
print "arr=", scalar(@a), "\n";

my %h = (a=>1, b=>2);
undef %h;
print "hash=", scalar(keys %h), "\n";

my $x = 1;
my $u = undef;
print "lit_def=", (defined $u ? 1 : 0), " assign=", (defined $x ? 1 : 0), "\n";
print "defor=", (undef // "x"), "\n";
print "defor2=", (1 // "y"), "\n";

# pidigits idiom
my $buf = "";
for my $i (1..20) {
    $buf .= "x";
    unless ($i % 10) { print "chunk=", length($buf), "\n"; undef $buf }
}
print "left=", length($buf // ""), "\n";

# %{EXPR} deref + statement-modifier if with &&
my $hr = {z => 9};
my %flat = %{ $hr };
print "flat=$flat{z}\n";
my $n = 0;
$n = 1 if (defined $hr) && $hr->{z};
print "modif=$n\n";
print "done\n";
