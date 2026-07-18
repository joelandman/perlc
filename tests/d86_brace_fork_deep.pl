# D86: print {EXPR} / say {EXPR} comprehensive tests
my $tmp1 = "/tmp/d86_deep.txt";
my $tmp2 = "/tmp/d86_deep2.txt";

# 1. print {EXPR} simple string
open(my $fh1, ">", $tmp1) or die;
print {$fh1} "abc\n";
close($fh1);

open(my $r1, "<", $tmp1) or die;
my $r1c = do { local $/; <$r1> }; close($r1);
if ($r1c ne "abc\n") { die "test1: got [$r1c]\n"; }
print "1: ok\n";

# 2. say {EXPR} adds trailing newline - use shell to compare
# We just check the file exists and write correctly
open(my $fh2, ">>", $tmp1) or die;
say {$fh2} "def";
close($fh2);

# Use shell command to check file content
my $shell_result = `cat $tmp1 | wc -c`;
chomp $shell_result;
if ($shell_result ne "8") { die "test2: file size $shell_result expected 8\n"; }
print "2: ok\n";

# 3. print {EXPR} with multiple args
open(my $fh3, ">", $tmp2) or die;
print {$fh3} "a", " ", "b", "\n";
close($fh3);

my $s3 = `cat $tmp2 | wc -c`;
chomp $s3;
if ($s3 ne "4") { die "test3: file size $s3 expected 4\n"; }
print "3: ok\n";

# 4. Variable as filehandle expression
open(my $fh4, ">>", $tmp2) or die;
my $fh_var = $fh4;
print {$fh_var} "e\n";
say {$fh_var} "f";
close($fh4);

my $s4 = `cat $tmp2 | wc -c`;
chomp $s4;
if ($s4 ne "8") { die "test4: file size $s4 expected 8\n"; }
print "4: ok\n";

print "all ok\n";
