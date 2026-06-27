use feature "say";
#!/usr/bin/perl
use strict;
use warnings;

# ── basic expression eval ────────────────────────────────────────────────────
my $result = eval "1 + 2";
print "$result\n";                        # 3

$result = eval '"hello"';
print "$result\n";                        # hello

# ── $@ is clear on success ───────────────────────────────────────────────────
eval "1 + 1";
print defined($@) && $@ eq "" ? "ok\n" : "fail\n";   # ok

# ── die sets $@ ──────────────────────────────────────────────────────────────
eval 'die "something went wrong"';
print $@ =~ /something went wrong/ ? "caught\n" : "fail\n";  # caught

# ── string interpolation before eval ─────────────────────────────────────────
my $x = 42;
$result = eval "$x * 2";
print "$result\n";                        # 84

my $op = "print";
eval "$op \"interpolated\\n\"";           # interpolated

# ── eval with print ───────────────────────────────────────────────────────────
eval 'print "from eval\n"';              # from eval

# ── eval with variables and control flow ─────────────────────────────────────
$result = eval 'my $s = 0; for my $i (1..5) { $s += $i } $s';
print "$result\n";                        # 15

# ── eval with sub definition and call ────────────────────────────────────────
eval 'sub greet { "Hello, $_[0]!" } print greet("world"), "\n"';  # Hello, world!

# ── nested eval ──────────────────────────────────────────────────────────────
eval 'eval "print \\"nested\\n\\"" ';    # nested

# ── eval parse error sets $@ ─────────────────────────────────────────────────
eval 'this is not valid perl ???';
print $@ ne "" ? "parse error caught\n" : "fail\n";   # parse error caught

# ── runtime error in eval ────────────────────────────────────────────────────
eval 'die "runtime error"';
print $@ =~ /runtime error/ ? "runtime caught\n" : "fail\n";   # runtime caught

# ── eval in loop ─────────────────────────────────────────────────────────────
my $sum = 0;
for my $n (1..3) {
    $sum += eval "$n * $n";
}
print "$sum\n";                           # 14  (1+4+9)

print "done\n";
