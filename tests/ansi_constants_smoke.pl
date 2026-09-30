# D154 smoke: Term::ANSIColor :constants (was not imported at all —
# `print BOLD, "x", RESET` printed the literal words), constants applied to
# a list, and uncolor/colorstrip/colorvalid (were undefined subs).
use Term::ANSIColor qw(:constants uncolor colorstrip colorvalid);
sub vis { my $s = shift; $s =~ s/\e/\\e/g; return $s }
print vis(BOLD . "a" . RESET), "\n";
print vis(BOLD "b"), "\n";
print vis(BOLD RED "c"), "\n";
print join(",", uncolor("\e[1;31m")), "\n";
print colorstrip("\e[1mplain\e[0m"), "\n";
print colorvalid("bold red") ? "valid\n" : "invalid\n";
