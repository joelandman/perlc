#!/usr/bin/perl
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

open my $fh, '>', 'test_do_module.pl' or die "Cannot create test file: $!";
print $fh <<'EOF';
package TestDoModule;
sub test_function {
    return 42;
}
1;
EOF
close $fh;

my $result = do './test_do_module.pl';
check('do_return_value', defined($result) && $result == 1);
check('do_loaded_function', TestDoModule::test_function() == 42);

my $missing = do './test_do_missing.pl';
check('do_missing_returns_undef', !defined($missing));
check('do_missing_sets_error', defined($@));

open $fh, '>', 'test_do_parse_error.pl' or die "Cannot create parse error file: $!";
print $fh "this is not valid perl ???\n";
close $fh;

my $parse = do './test_do_parse_error.pl';
check('do_parse_error_returns_undef', !defined($parse));
check('do_parse_error_sets_error', defined($@) && length($@) > 0);

open $fh, '>', 'test_do_runtime_die.pl' or die "Cannot create runtime error file: $!";
print $fh "die \"boom from do\\n\";\n";
close $fh;

my $runtime = do './test_do_runtime_die.pl';
check('do_runtime_die_returns_undef', !defined($runtime));
check('do_runtime_die_sets_error', defined($@) && $@ =~ /boom from do/);

unlink 'test_do_module.pl';
unlink 'test_do_parse_error.pl';
unlink 'test_do_runtime_die.pl';

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "test_do_filename_done\n";
