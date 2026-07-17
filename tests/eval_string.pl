use feature "say";
#!/usr/bin/perl
use strict;
use warnings;

# ── Test eval { BLOCK } — works in both perl and perlc ─────────────────────
my $result = eval { 1 + 2 };
print "block_eval=", $result, "\n";  # 3

eval { die "caught error\n"; };
print "caught=", ($@ ? "yes" : "no"), "\n";  # yes

eval { my $ok = 1; };
print "clean_eval=", $@ eq "" ? "ok" : "fail", "\n";  # ok

eval { die "boom\n"; };
if ($@) { print "got: $@"; }  # got: boom\n

# ── String eval note ───────────────────────────────────────────────────────
# String eval (eval EXPR) is not supported in perlc (removed with JIT).
# Both perl and perlc produce the same eval { BLOCK } output above.
print "eval_string_done\n";
