#!/usr/bin/perl
# Test D36 residual: known subs called without parens should work

sub FOO { return "hello" }
sub BAR { return 42 }

# Test 1: Known sub without parens in assignment
my $x = FOO;
die "Test 1 failed" unless $x eq "hello";

# Test 2: Known sub without parens in print
my $out = '';
open(my $fh, '>', '/tmp/d36_test_out.txt') or die;
print $fh FOO;
close($fh);
open(my $fh2, '<', '/tmp/d36_test_out.txt') or die;
my $printed = do { local $/; <$fh2> };
close($fh2);
die "Test 2 failed" unless $printed eq "hello";

# Test 3: Hash with bareword keys (should NOT be treated as sub calls)
my %h = (red => 1, green => 2, blue => 3);
die "Test 3a failed" unless $h{red} == 1;
die "Test 3b failed" unless $h{green} == 2;

# Test 4: Hash access with bareword key
die "Test 4 failed" unless $h{blue} == 3;

# Test 5: exists with bareword key
die "Test 5a failed" unless exists $h{red};
die "Test 5b failed" if exists $h{yellow};

# Test 6: delete with bareword key
$h{red} = 10;
delete $h{red};
die "Test 6 failed" if exists $h{red};

# Test 7: Multiple subs
my $a = FOO;
my $b = BAR;
die "Test 7 failed" unless $a eq "hello" && $b == 42;

# Test 8: Sub with args still works
sub ADD { return $_[0] + $_[1] }
my $sum = ADD(10, 20);
die "Test 8 failed" unless $sum == 30;

# Test 9: Sub without parens in string interpolation
my $str = "Result: $x";
die "Test 9 failed" unless $str eq "Result: hello";

print "All D36 residual tests passed\n";
