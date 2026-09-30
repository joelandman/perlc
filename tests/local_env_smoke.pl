local $ENV{PERLC_LOCAL_ENV_TEST} = "hello";
print $ENV{PERLC_LOCAL_ENV_TEST}, "\n";
print $ENV{PERLC_LOCAL_ENV_TEST} eq "hello" ? "ok\n" : "FAIL\n";
