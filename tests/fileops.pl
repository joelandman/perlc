use lib "tests/lib";
use feature "say";
use MathOps;               # imports 'add' via @EXPORT
use MathOps qw(subtract);  # imports 'subtract' via @EXPORT_OK (already loaded)
use MathOps qw(multiply);  # imports 'multiply' via @EXPORT_OK (already loaded)

use constant FACTOR => 2;
use constant {
    X => 10,
    Y => 20,
};

# imported functions
print add(3, 4) . "\n";       # 7
print subtract(10, 3) . "\n"; # 7
print multiply(3, 4) . "\n";  # 12

# module constants (bare names)
print PI . "\n";     # 3.14159
print MAX . "\n";    # 100

# local use constant
print FACTOR . "\n"; # 2
print X . "\n";      # 10
print Y . "\n";      # 20

# directory operations
my $dir = "/tmp/perlc_fo_$$";
mkdir($dir);

open(my $fa, ">", "$dir/a.txt") or die "open a: $!";
close($fa);
open(my $fb, ">", "$dir/b.txt") or die "open b: $!";
close($fb);

opendir(my $dh, $dir) or die "opendir: $!";
my @files = sort grep { $_ ne '.' && $_ ne '..' } readdir($dh);
closedir($dh);
print scalar(@files) . "\n";   # 2
print join(",", @files) . "\n"; # a.txt,b.txt

rename("$dir/a.txt", "$dir/c.txt");

opendir(my $dh2, $dir) or die "opendir2: $!";
my @files2 = sort grep { $_ ne '.' && $_ ne '..' } readdir($dh2);
closedir($dh2);
print join(",", @files2) . "\n"; # b.txt,c.txt

unlink("$dir/b.txt");
unlink("$dir/c.txt");
rmdir($dir);
print "done\n";
