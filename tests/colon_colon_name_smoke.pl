# D195: ::NAME is main::NAME (zipdetails).
use constant SIG => 42;
sub hello { print "hi\n" }
package Other;
my %h = (::SIG, "v");
print "$h{42}\n";
::hello();
