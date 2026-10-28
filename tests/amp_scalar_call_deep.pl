# D196 deep: `&$scalar` — call a code reference stored in a scalar variable.
# Covers various shapes of the pattern.

use strict;
use warnings;

my @results;

sub greet {
    my ($name) = @_;
    return "Hello, $name!";
}

sub add {
    my ($a, $b) = @_;
    return $a + $b;
}

sub concat {
    my ($a, $b) = @_;
    return "$a$b";
}

# Basic &-$scalar call
my $greet_ref = \&greet;
push @results, &$greet_ref("World");

# &-$scalar with multiple args
my $add_ref = \&add;
push @results, &$add_ref(10, 20);

# &-$scalar in expression context
my $concat_ref = \&concat;
my $x = "hi";
my $y = " there";
push @results, &$concat_ref($x, $y);

# &-$scalar with if modifier (common splain pattern)
my $condition = 1;
my $result;
&$greet_ref("Test") if $condition;

# &-$scalar assigned to variable
my $captured = &$greet_ref("Alice");
push @results, $captured;

# &-$scalar inside array context
my @greetings = (&$greet_ref("A"), &$greet_ref("B"), &$greet_ref("C"));
push @results, join(", ", @greetings);

# &-$scalar with return value used in arithmetic
my $sum = &$add_ref(1, 2) + &$add_ref(3, 4);
push @results, $sum;

# &-$scalar with blessed object method (indirect call)
package MyPkg;
sub method {
    my ($self, $val) = @_;
    return "method called with $val";
}
package main;

my $obj = bless({}, "MyPkg");
my $method_ref = $obj->can("method");
if ($method_ref) {
    push @results, &$method_ref($obj, "test");
}

# &-$scalar with code ref from anonymous sub
my $anon = sub { return "anonymous"; };
push @results, &$anon();

print join("\n", @results), "\n";
