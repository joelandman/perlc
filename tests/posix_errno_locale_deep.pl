#!/usr/bin/perl
# Deep test for D162: a batch of pre-existing gaps found via two real
# scripts (/usr/bin/dpkg-genchanges: `use POSIX qw(:errno_h
# :locale_h);` then `setlocale(LC_TIME, 'C')`; /usr/bin/dpkg-
# buildpackage: `use POSIX qw(:sys_wait_h);` then `WIFEXITED($status)`/
# `WEXITSTATUS($status)`).
#
# 1. POSIX's native constant-export mechanism (src/main.cpp) only knew
#    about a handful of hand-picked tags/names — `:errno_h`,
#    `:locale_h`, and `:sys_wait_h` weren't recognized at all, dying
#    "is not defined in %EXPORT_TAGS of the POSIX module" on the `use`
#    line itself, before any of the script's own logic ever ran.
#    Added the full real-Perl tag lists (probed from the host perl),
#    deliberately omitting the handful of names (LC_SYNTAX/LC_TOD,
#    EOTHER/EPROCLIM) that aren't defined on this platform in real
#    Perl either.
# 2. errno()/setlocale()/localeconv() (real functions, not constants)
#    and the sys_wait_h status macros (WIFEXITED/WEXITSTATUS/
#    WIFSIGNALED/WTERMSIG/WIFSTOPPED/WSTOPSIG) weren't implemented at
#    all. Added as thin wrappers over the real C library (errno,
#    setlocale(3), localeconv(3), and the real WIFEXITED/WEXITSTATUS/
#    etc. macros from sys/wait.h).
# 3. Found while verifying errno(): `$! = N` assignment was a
#    completely separate, pre-existing, deeper bug — it wrote into
#    perl_get_dollar_bang's internal cell via plain perl_assign, but
#    that cell is unconditionally *recomputed from the live OS errno*
#    on every subsequent read, so the assignment never actually
#    persisted past the very next read of $!, and never touched the
#    real OS errno at all (so errno(), which reads the OS value
#    directly, never saw it either). Fixed with a dedicated
#    perl_set_dollar_bang() that sets the real errno global.
use strict;
use warnings;
use POSIX qw(:errno_h :locale_h :sys_wait_h);

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# errno_h / locale_h / sys_wait_h tags import without dying, and the
# constants resolve to their real values
check('errno_h_einval', EINVAL == 22);
check('errno_h_enoent', ENOENT == 2);
check('locale_h_lc_time', LC_TIME() >= 0);
check('sys_wait_h_wnohang', WNOHANG() == 1);
check('sys_wait_h_wuntraced', WUNTRACED() == 2);

# setlocale: real function, not a constant
{
    my $old = setlocale(LC_TIME, 'C');
    check('setlocale_returns_defined', defined($old));
}

# localeconv: returns a hashref with the expected keys
{
    my $lc = localeconv();
    check('localeconv_is_hashref', ref($lc) eq 'HASH');
    check('localeconv_has_decimal_point', exists $lc->{decimal_point});
}

# $! = N sets the real OS errno (D162's own deeper finding) — a plain
# subsequent read of $! sees the change, and so does errno()
{
    $! = 2;
    check('dollar_bang_after_set', "$!" eq "No such file or directory");
    my $e = errno();
    check('errno_matches_dollar_bang_set', $e == 2);
}
{
    $! = 0;
    check('dollar_bang_zero', "$!" eq "");
    my $e = errno();
    check('errno_matches_dollar_bang_zero', $e == 0);
}

# WIFEXITED/WEXITSTATUS on a real child process exit status
{
    system("true") if -x "/bin/true" || -x "/usr/bin/true";
    my $status = $?;
    check('wifexited_true', WIFEXITED($status) ? 1 : 0);
    check('wexitstatus_true_is_zero', WEXITSTATUS($status) == 0);
}
{
    system("false") if -x "/bin/false" || -x "/usr/bin/false";
    my $status = $?;
    check('wifexited_false', WIFEXITED($status) ? 1 : 0);
    check('wexitstatus_false_is_one', WEXITSTATUS($status) == 1);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "posix_errno_locale_done\n";
