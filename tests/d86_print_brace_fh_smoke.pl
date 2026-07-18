# D86: print {EXPR} LIST brace-block filehandle smoke test
# (no use warnings to avoid diagnostic differences, D56)

# Test 1: Basic brace-block filehandle
open(my $fh, '>', '/tmp/d86_test1.txt') or die;
print {$fh} "hello\n";
close($fh);
open(my $fh1, '<', '/tmp/d86_test1.txt') or die;
my $c1 = <$fh1>;
close($fh1);
die "D86: test1" unless $c1 eq "hello\n";

# Test 2: Multiple arguments
open(my $fh2, '>', '/tmp/d86_test2.txt') or die;
print {$fh2} "a", " ", "b", "\n";
close($fh2);
open(my $fh3, '<', '/tmp/d86_test2.txt') or die;
my $c2 = <$fh3>;
close($fh3);
die "D86: test2" unless $c2 eq "a b\n";

# Test 3: Scalar var as filehandle
open(my $fh4, '>', '/tmp/d86_test3.txt') or die;
my $fh5 = $fh4;
print {$fh5} "world\n";
close($fh4);
open(my $fh6, '<', '/tmp/d86_test3.txt') or die;
my $c3 = <$fh6>;
close($fh6);
die "D86: test3" unless $c3 eq "world\n";

print "ok\n";
