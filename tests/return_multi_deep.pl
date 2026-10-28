# D197 deep: unparenthesized multi-value return — `return A, B, C`
# Covers various shapes of multi-value return statements.

use strict;
use warnings;

my @results;

# Basic unparenthesized multi-value return
sub basic { return 1, 2, 3; }
push @results, join(",", &basic);

# Unparenthesized with empty strings
sub empties { return '', '', 0; }
my @e = &empties;
push @results, scalar(@e) . "=" . join("|", @e);

# Unparenthesized with mixed types
sub mixed { return "a", 42, "c"; }
push @results, join(",", &mixed);

# Unparenthesized with two values
sub two { return "x", "y"; }
push @results, join(",", &two);

# Unparenthesized single value (should NOT be affected)
sub single { return "only"; }
push @results, &single;

# Unparenthesized single value with modifier
sub single_mod { return "mod" if 1; }
push @results, &single_mod;

# Unparenthesized with numeric values
sub nums { return 10, 20, 30, 40; }
push @results, join(",", &nums);

# Unparenthesized with expressions
sub exprs { return 1 + 1, 2 * 3, 4 ** 2; }
push @results, join(",", &exprs);

# Unparenthesized returning from nested sub
sub outer {
    sub inner { return "inner_a", "inner_b"; }
    return &inner;
}
push @results, join(",", &outer);

# Unparenthesized with array element
sub with_arr {
    my @arr = (1, 2, 3);
    return $arr[0], $arr[1], $arr[2];
}
push @results, join(",", &with_arr);

# Unparenthesized with hash values
sub with_hash {
    my %h = (a => 1, b => 2);
    return $h{a}, $h{b};
}
push @results, join(",", &with_hash);

# Multiple unparenthesized returns in same sub (different paths)
sub conditional {
    my ($flag) = @_;
    if ($flag) {
        return "yes", "true";
    } else {
        return "no", "false";
    }
}
push @results, join(",", &conditional(1));
push @results, join(",", &conditional(0));

print join("\n", @results), "\n";
