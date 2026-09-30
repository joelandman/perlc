# D154 deep: Term::ANSIColor :constants / :pushpop tags, constants with and
# without arguments, AUTORESET, nesting, and uncolor/colorstrip/colorvalid
# in list and scalar context.
use strict;
use Term::ANSIColor qw(:constants :pushpop uncolor colorstrip colorvalid colored color);
sub vis { my $s = join("", @_); $s =~ s/\e/\\e/g; return $s }

# bare constants in every position
print vis(BOLD, "a", RESET), "\n";
my $s = BOLD . "b" . RESET;
print vis($s), " len=", length($s), "\n";
my @codes = (CLEAR, RESET, BOLD, DARK, FAINT, ITALIC, UNDERLINE, UNDERSCORE,
             BLINK, REVERSE, CONCEALED);
print vis(@codes), "\n";
print vis(BLACK, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE), "\n";
print vis(ON_BLACK, ON_RED, ON_GREEN, ON_YELLOW, ON_BLUE, ON_MAGENTA, ON_CYAN, ON_WHITE), "\n";
print vis(BRIGHT_BLACK, BRIGHT_RED, BRIGHT_GREEN, BRIGHT_YELLOW,
          BRIGHT_BLUE, BRIGHT_MAGENTA, BRIGHT_CYAN, BRIGHT_WHITE), "\n";
print vis(ON_BRIGHT_BLACK, ON_BRIGHT_RED, ON_BRIGHT_GREEN, ON_BRIGHT_YELLOW,
          ON_BRIGHT_BLUE, ON_BRIGHT_MAGENTA, ON_BRIGHT_CYAN, ON_BRIGHT_WHITE), "\n";

# constants applied to a list, nested
print vis(BOLD "x"), "\n";
print vis(BOLD "x", "y", "z"), "\n";
print vis(BOLD RED "nested"), "\n";
print vis(UNDERLINE BRIGHT_WHITE ON_BLACK "deep", RESET), "\n";
my $n = 3;
print vis(GREEN "n=$n"), "\n";
print vis(BOLD("paren")), "\n";

# hash keys stay strings
my %h = (BOLD => 1, RED => 2);
print join(",", sort keys %h), " ", $h{BOLD}, "\n";

# AUTORESET appends a reset only when there are arguments
{
    local $Term::ANSIColor::AUTORESET = 1;
    print vis(BOLD "auto"), "\n";
    print vis(BOLD, "noargs"), "\n";
}
print vis(BOLD "after"), "\n";

# uncolor
print join(",", uncolor("\e[0;1;2;4;31;92;104m")), "\n";
print join(",", uncolor("1;31", "\e[44m")), "\n";
my $cnt = uncolor("1;31");
print "count=$cnt\n";
my @none = uncolor("\e[m");
print "none=", scalar(@none), "\n";
eval { uncolor("\e[99m") }; print "err1: $@";
eval { uncolor("xyz") };    print "err2: $@";

# colorstrip
my @plain = colorstrip("\e[1ma\e[0m", "b\e[31;1m", "c");
print join("|", @plain), "\n";
my $joined = colorstrip("\e[1mA", "\e[0mB");
print "$joined\n";
print colorstrip(colored("wrapped", "bold blue")), "\n";

# colorvalid
print colorvalid("bold red") ? 1 : 0, colorvalid("blorp") ? 1 : 0,
      colorvalid("on_bright_cyan", "underline") ? 1 : 0, "\n";
print defined(colorvalid("nope")) ? "d" : "u", " ", scalar(() = colorvalid("nope")), "\n";

# existing color()/colored() unaffected
print vis(color("bold red")), " ", vis(colored("t", "green")), "\n";
