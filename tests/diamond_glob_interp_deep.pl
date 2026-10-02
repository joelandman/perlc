# Deep test for D173: diamond-glob <PATTERN>'s pattern text was NOT
# variable-interpolated at all -- D164's own original write-up claimed
# this was real Perl's actual behavior ("the pattern text is a
# literal, not variable-interpolated"), but that claim was wrong/
# under-verified: real Perl DOES interpolate it, confirmed directly.
# Found via a real /usr/sbin/update-rc.d script:
# `my @links=<"$dpkg_root/etc/rc[S12345].d/S[0-9][0-9]$scriptname">;`
# -- the quoted form, which ALSO needed the lexer's glob-pattern
# safe-char set widened (it previously aborted its scan on the
# opening '"', falling back to a bare '<' token and a parse error).
my $dir = "/tmp/perlc_d173_deep";
mkdir $dir;
for my $n (1..3) {
    open(my $fh, ">", "$dir/f$n.tmp") or die $!;
    close $fh;
}

# unquoted interpolated form: <$dir/*.tmp>
my @unquoted = sort <$dir/*.tmp>;
print "unquoted_count=", scalar(@unquoted), "\n";

# double-quoted interpolated form: <"$dir/*.tmp"> (the exact
# real-world update-rc.d shape)
my @dquoted = sort <"$dir/*.tmp">;
print "dquoted_count=", scalar(@dquoted), "\n";

# single-quoted form: <'$dir/*.tmp'> -- real Perl interpolates this
# too (verified directly; the diamond-glob form doesn't carry the
# usual single-quote-means-no-interpolation distinction)
my @squoted = sort <'$dir/*.tmp'>;
print "squoted_count=", scalar(@squoted), "\n";

# a pattern built from an interpolated variable plus a bracket-class
# glob metacharacter immediately after a LITERAL (non-variable) path
# segment (the exact real-world update-rc.d shape:
# "$var1/rc[S12345].d/S[0-9][0-9]$var2") -- avoiding a bracket
# directly after a bare scalar name (`$prefix[0-9]`), which real Perl
# interpolation treats as array-element access, not literal-text-
# then-bracket-glob; not this fix's concern.
my $suffix = "tmp";
my @mixed = sort <"$dir/f[0-9].$suffix">;
print "mixed_count=", scalar(@mixed), "\n";

# scalar context: must re-interpolate correctly across repeated calls
# and still iterate one match per call (not break the D164 iterator
# fix)
my $n = 0;
while (my $f = <$dir/*.tmp>) {
    $n++;
}
print "scalar_iter_count=$n\n";

# a pattern that interpolates to something matching zero files: empty
# list, not an error
my $nomatch = "zzz_nomatch";
my @none = <$dir/$nomatch*.tmp>;
print "empty_count=", scalar(@none), "\n";

# regression: a literal (non-interpolated) pattern must still work
# exactly as before
my @literal = sort </tmp/perlc_d173_deep/*.tmp>;
print "literal_count=", scalar(@literal), "\n";

for my $n (1..3) { unlink "$dir/f$n.tmp"; }
rmdir $dir;
