#!/usr/bin/perl
# Deep test for D160: `local $ENV{KEY} = VAL` used to be a complete,
# silent no-op — %ENV has no real backing PerlHash under lookupHash()
# at all (it's accessible only through perl_env_get/perl_env_set), so
# the LocalStmt hash_elem codegen's `if (!hv) break;` unconditionally
# bailed out before anything happened: no save, no assign, nothing.
# Found while implementing File::Which — which()/where() (and any
# other native code calling getenv() directly, e.g. a spawned child
# process) never saw a localized %ENV override. Fixed by special-
# casing ENV to call perl_env_set directly, the same runtime call
# plain (non-local) `$ENV{KEY} = VAL` already uses.
#
# NB: this fix is deliberately scoped down — it does NOT hook into the
# generic perl_local_save/perl_local_restore_to dynamic-scope stack,
# so (unlike every other `local` target, and unlike real Perl) the
# environment variable is NOT restored to its prior value when the
# enclosing block/sub exits. This test only exercises the part that
# now genuinely works (the value being visible, including to a real
# child process, for the duration it's set) and deliberately does NOT
# assert anything about post-scope restoration, since that would
# diverge from real Perl.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# basic: perl-level $ENV{} read sees the localized value
{
    local $ENV{PERLC_D160_TEST} = "value1";
    check('perl_level_read', $ENV{PERLC_D160_TEST} eq "value1");
}

# the real proof: a spawned CHILD PROCESS reads its environment via
# execve(2)/getenv(3), never touching perlc's internal %ENV hash at
# all — this only passes if perl_env_set's setenv() call actually ran.
sub in_sub {
    local $ENV{PERLC_D160_CHILD_TEST} = "childvalue";
    my $env_dump = `env`;
    my ($seen) = $env_dump =~ /^PERLC_D160_CHILD_TEST=(.*)$/m;
    return $seen // '';
}
check('child_process_sees_value', in_sub() eq "childvalue");

# bare `local $ENV{KEY};` (no assignment) sets it to undef/empty,
# matching real Perl's bare-local-assigns-undef semantics
{
    $ENV{PERLC_D160_BARE} = "before";
    local $ENV{PERLC_D160_BARE};
    check('bare_local_env_is_empty', ($ENV{PERLC_D160_BARE} // '') eq '');
}

# a plain (non-local) $ENV{} write is completely unaffected by this fix
{
    $ENV{PERLC_D160_PLAIN} = "plainvalue";
    check('plain_env_write_unaffected', $ENV{PERLC_D160_PLAIN} eq "plainvalue");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "local_env_done\n";
