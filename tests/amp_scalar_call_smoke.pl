# D196: `&$scalar` — call a code reference stored in a scalar variable.
# Real Perl: `&$scalar` calls the subroutine referenced by $scalar.
# perlc previously: "unexpected token '&'".
# Found verbatim in /usr/bin/splain: `&$olddie if defined $olddie`.

my @results;

sub mysub { return "hello"; }
sub add { my ($a, $b) = @_; return $a + $b; }

my $ref = \&mysub;
push @results, &$ref;

my $adder = \&add;
push @results, &$adder(3, 4);

# &-$scalar with modifier (the splain pattern)
my $cond = 1;
&$ref if $cond;

# &-$scalar assigned to variable
my $captured = &$ref;
push @results, $captured;

# &-$scalar with args inside expression
my $x = &$adder(10, 20);
push @results, $x;

print join("\n", @results), "\n";
