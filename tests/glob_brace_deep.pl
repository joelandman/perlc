# Deep test for D185: the diamond-glob `<PATTERN>` syntax and the
# plain `glob()` builtin didn't brace-expand `{a,b,c}` alternation at
# all -- `<*.{gz,txt}>` only ever matched a literal "*.{gz,txt}"
# filename (never found, so silently empty), instead of real Perl's
# default brace-expansion behavior. Needed two fixes: (1) the lexer's
# diamond-glob safe-character allowlist didn't include `{`/`}`, so a
# pattern containing them fell back to a bare `<` token and a
# confusing parse error rather than even reaching the glob call; (2)
# perl_glob_val (the runtime behind both `glob()` and `<PATTERN>`)
# never passed GLOB_BRACE to the underlying glob(3) call. Found via a
# real /usr/bin/helpztags script: `foreach my $file (<*.{gz,txt,??x}>)`.

my @names = ('glob_brace_deep_a.gz', 'glob_brace_deep_b.txt',
             'glob_brace_deep_c.foo', 'glob_brace_deep_d.tarx');
for my $n (@names) {
    open(my $fh, '>', $n) or die "open $n: $!";
    close $fh;
}

print "diamond=", join(",", sort <glob_brace_deep_*.{gz,txt}>), "\n";
print "builtin_glob=", join(",", sort glob("glob_brace_deep_*.{gz,txt}")), "\n";

# three-way alternation
print "three_way=", join(",", sort <glob_brace_deep_*.{gz,txt,foo}>), "\n";

# no match on the brace-expanded set -> empty list
print "no_match=", join(",", <glob_brace_deep_*.{zzz,yyy}>), "\n";

# regression: non-brace patterns still work
print "plain=", join(",", sort <glob_brace_deep_*.txt>), "\n";

# regression: ordinary hash-element-in-comparison parsing is unaffected
# now that { and } are glob-safe characters
my %h = (k => 3);
print "hashcmp=", (($h{k} < 5 && 1 > 0) ? "ok" : "bad"), "\n";

unlink @names;

print "done\n";
