package MathOps;

our @EXPORT    = qw(add);
our @EXPORT_OK = qw(subtract multiply);

use constant PI  => 3.14159;
use constant MAX => 100;

sub add      { my ($a, $b) = @_; return $a + $b; }
sub subtract { my ($a, $b) = @_; return $a - $b; }
sub multiply { my ($a, $b) = @_; return $a * $b; }

1;
