package D26Const;
use Exporter 'import';
our @EXPORT    = ('EXPORTED_C');
our @EXPORT_OK = ('OK_C');

use constant EXPORTED_C => 111;
use constant OK_C       => 222;
use constant PRIV_C     => 333;
use constant {
    PRIV_HASH_A => 44,
    PRIV_HASH_B => 55,
};

1;
