package LexerWrapper;
use strict;

use base qw(Exporter);

our @EXPORT_OK = qw(lex);

use File::Temp;
use YAML qw(Load);

chomp(my $here = qx(dirname $0));
my $executable = qq($here/lexer);

sub lex {
    my $temp = File::Temp->new;
    print $temp @_;
    close $temp;
    return Load(scalar qx/$executable $temp/);
}

1;

