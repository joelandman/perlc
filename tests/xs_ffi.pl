#!/usr/bin/perl
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my $libc = XS::load_library("libc.so.6");
check('xs_load_libc', defined($libc));

my $strlen = XS::call($libc, "strlen", "long(string)", "hello");
check('xs_strlen_defined', defined($strlen));
check('xs_strlen_value', $strlen == 5);

my $strcmp = XS::call($libc, "strcmp", "long(string, string)", "perl", "perl");
check('xs_strcmp_defined', defined($strcmp));
check('xs_strcmp_value', $strcmp == 0);

my $strncmp = XS::call($libc, "strncmp", "long(string, string, long)", "alphabet", "alpha", 5);
check('xs_strncmp_defined', defined($strncmp));
check('xs_strncmp_value', $strncmp == 0);

my $strchr = XS::call($libc, "strchr", "string(string, long)", "hello", 108);
check('xs_strchr_defined', defined($strchr));
check('xs_strchr_value', $strchr eq 'llo');

my $strchr_buf = XS::call($libc, "calloc", "ptr(long, long)", 8, 1);
XS::call($libc, "strcpy", "ptr(ptr, string)", $strchr_buf, "hello");
my $strchr_ptr = XS::call($libc, "strchr", "ptr(ptr, long)", $strchr_buf, 108);
check('xs_strchr_ptr_defined', defined($strchr_ptr));
check('xs_strchr_ptr_type', Scalar::Util::reftype($strchr_ptr) eq 'PTR');
check('xs_strchr_ptr_strlen', XS::call($libc, "strlen", "long(ptr)", $strchr_ptr) == 3);
XS::call($libc, "free", "void(ptr)", $strchr_buf);

my $strchr_missing = XS::call($libc, "strchr", "ptr(string, long)", "hello", 122);
check('xs_strchr_missing_undef', !defined($strchr_missing));

my $srand_ok = XS::call($libc, "srand", "void(long)", 7);
my $rand1 = XS::call($libc, "rand", "long()");
my $rand2 = XS::call($libc, "rand", "long()");
XS::call($libc, "srand", "void(long)", 7);
my $rand3 = XS::call($libc, "rand", "long()");
check('xs_void_call_defined', !defined($srand_ok));
check('xs_rand_defined', defined($rand1) && defined($rand2) && defined($rand3));
check('xs_rand_reseed_value', $rand1 == $rand3 && $rand1 != $rand2);

my $buf = XS::call($libc, "calloc", "ptr(long, long)", 8, 1);
check('xs_calloc_defined', defined($buf));
check('xs_calloc_type', Scalar::Util::reftype($buf) eq 'PTR');
check('xs_calloc_truthy', $buf ? 1 : 0);

my $memset = XS::call($libc, "memset", "ptr(ptr, long, long)", $buf, 65, 4);
check('xs_memset_defined', defined($memset));
check('xs_memset_same_ptr', $memset == $buf);
check('xs_memset_strlen', XS::call($libc, "strlen", "long(ptr)", $buf) == 4);
check('xs_memset_strcmp', XS::call($libc, "strcmp", "long(ptr, string)", $buf, "AAAA") == 0);
check('xs_free_defined', !defined(XS::call($libc, "free", "void(ptr)", $buf)));

my $libm = XS::load_library("libm.so.6");
check('xs_load_libm', defined($libm));

my $fabs = XS::call($libm, "fabs", "double(double)", -3.5);
check('xs_fabs_defined', defined($fabs));
check('xs_fabs_value', $fabs > 3.49 && $fabs < 3.51);

my $pow = XS::call($libm, "pow", "double(double, double)", 2.0, 3.0);
check('xs_pow_defined', defined($pow));
check('xs_pow_value', $pow > 7.99 && $pow < 8.01);

my $fma = XS::call($libm, "fma", "double(double, double, double)", 2.0, 3.0, 4.0);
check('xs_fma_defined', defined($fma));
check('xs_fma_value', $fma > 9.99 && $fma < 10.01);

my $atof = XS::call($libc, "atof", "double(string)", "12.5");
check('xs_atof_defined', defined($atof));
check('xs_atof_value', $atof > 12.49 && $atof < 12.51);

my $pow_spaced = XS::call($libm, "pow", " double ( double , double ) ", 3.0, 2.0);
check('xs_signature_whitespace_defined', defined($pow_spaced));
check('xs_signature_whitespace_value', $pow_spaced > 8.99 && $pow_spaced < 9.01);

my $bad_symbol = XS::call($libc, "definitely_missing_symbol_xyz", "long()");
check('xs_bad_symbol_undef', !defined($bad_symbol));
check('xs_bad_symbol_sets_error', length($@) > 0);

my $bad_lib = XS::load_library("libdoesnotexist_perlc.so");
check('xs_bad_library_undef', !defined($bad_lib));
check('xs_bad_library_sets_error', length($@) > 0);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "xs_ffi_done\n";
