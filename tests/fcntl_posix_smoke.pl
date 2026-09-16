use Fcntl qw(:seek :flock :DEFAULT);
print SEEK_SET, " ", SEEK_CUR, " ", SEEK_END, "\n";
print Fcntl::O_CREAT, " ", Fcntl::O_RDONLY, "\n";
use POSIX qw(LC_ALL);
print POSIX::LC_ALL, "\n";
print "smoke_done\n";
