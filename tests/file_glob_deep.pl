use strict;
use warnings;
use File::Glob qw(:glob);

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# D157: perl_bsd_glob_val (bsd_glob) and the core glob() builtin's
# shared perl_glob_val both went through this fix. Root causes:
# (1) perl_glob_val hardcoded GLOB_NOCHECK, so both glob() and (before
#     this fix existed) bsd_glob returned the literal pattern string on
#     no match instead of real Perl's actual empty-list result.
# (2) bsd_glob's FLAGS argument was entirely ignored. Real File::Glob
#     ships its own bundled BSD-glob implementation with its own
#     GLOB_* bit values that do NOT match glibc's glob.h numbering
#     (verified: Perl's GLOB_TILDE=2048 vs glibc's 1<<12=4096,
#     GLOB_BRACE=128 vs glibc's 1<<10=1024) — perl_bsd_glob_flags_to_libc
#     translates the subset glibc's glob(3) actually implements
#     (TILDE, BRACE, MARK, NOSORT, ERR, NOCHECK, NOMAGIC) from Perl's
#     own flag numbering. GLOB_NOCASE and a few rarer flags
#     (ALPHASORT, ALTDIRFUNC, LIMIT, QUOTE, CSH, ABEND) are accepted
#     (compile and run) but not honored, since glibc has no
#     case-insensitive glob primitive to build on — not exercised here.
my $dir = "/tmp/perlc_fglob_deep";
mkdir $dir;
for my $f (qw(one.txt two.txt three.log)) {
    open(my $fh, '>', "$dir/$f") or die $!;
    close $fh;
}

# basic list-context match
my @txt = sort(bsd_glob("$dir/*.txt"));
check('basic_list_match_count', scalar(@txt) == 2);
check('basic_list_contents', join(",", map { (split m{/})[-1] } @txt) eq "one.txt,two.txt");

# scalar context: first match (not a full-list collapse)
my $s = bsd_glob("$dir/*.log");
check('scalar_context_single', $s eq "$dir/three.log");

# no match: empty list (the D157 fix — not the literal pattern)
my @none = bsd_glob("$dir/*.nomatch");
check('no_match_is_empty', scalar(@none) == 0);

# core glob() builtin gets the identical no-match fix
my @none2 = glob("$dir/*.alsonomatch");
check('core_glob_no_match_is_empty', scalar(@none2) == 0);

# GLOB_BRACE: brace expansion
open(my $afh, '>', "$dir/alpha.dat") or die $!;
close $afh;
open(my $bfh, '>', "$dir/beta.dat") or die $!;
close $bfh;
my @braced = sort(bsd_glob("$dir/{alpha,beta}.dat", GLOB_BRACE));
check('glob_brace', join(",", map { (split m{/})[-1] } @braced) eq "alpha.dat,beta.dat");

# no flags argument at all (the common, simplest call shape)
my @plain = bsd_glob("$dir/one.txt");
check('no_flags_arg', "@plain" eq "$dir/one.txt");

# constants: values match real Perl's own File::Glob (probed, not
# glibc's — see tools/gen_native_constants.pl)
check('const_glob_tilde', GLOB_TILDE() == 2048);
check('const_glob_brace', GLOB_BRACE() == 128);
check('const_glob_nocase', GLOB_NOCASE() == 4096);
check('const_glob_err', GLOB_ERR() == 4);

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "file_glob_done\n";
