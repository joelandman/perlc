# D190: trailing comma as the last statement of a block (crontab2english).
our @x;
sub f {
    push @x, sprintf("%s", "a",),
}
f();
print $x[0], "\n";
