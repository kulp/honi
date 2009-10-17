package ParserWrapper;
use strict;

use base qw(Exporter);

our @EXPORT_OK = qw(hd2yaml);

use File::Temp;
use YAML qw(Load);

chomp(my $here = qx(dirname $0));
my $executable = qq($here/hd2yaml);

sub hd2yaml {
    my $temp = File::Temp->new;
    print $temp @_;
    close $temp;
    return Load(scalar qx/$executable $temp/);
}

1;

