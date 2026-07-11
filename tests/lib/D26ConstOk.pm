package D26ConstOk;
use Exporter 'import';
our @EXPORT_OK = ('OK_ONLY');

use constant OK_ONLY   => 777;
use constant PRIV_ONLY => 888;

1;
