#!/usr/bin/env perl
use strict;
use warnings;
use feature 'say';

package Fortyn;

use Data::HexDump;
use Digest::MD5 qw(md5_hex);
use IO::Socket::INET;
use List::MoreUtils qw(mesh);
use LWP::UserAgent;
use YAML qw(Dump);

use POE qw(Component::Client::TCP Filter::Stream);

use lib ".";
use LexerWrapper qw(lex);

################################################################################

my $base = qq(http://masterserver.hon.s2games.com);
my $cr_url = qq($base/client_requester.php);

my $keepalive_period = 30;  # seconds
my $home_channel = "UWEC";

my ($user, $pass) = @ARGV;
$user and $pass or die "Supply username and password";

# TODO move into object
my $ua = LWP::UserAgent->new;

################################################################################

my %actions = (
    0x00 => 'login_success',
    0x04 => 'channel_presence',
    0x0B => 'my_presence',
    0x2D => 'whois_response',
    0x03 => 'channel_traffic',

    0x05 => 'ignore_message',
    0x06 => 'ignore_message',
    0x0C => 'ignore_message',
    0x18 => 'ignore_message',
);

################################################################################

my $client = Fortyn->new;
POE::Kernel->run();
exit;

################################# M E T H O D S ################################
#
sub new
{
    my $class = shift;
    my $self = bless {
        @_,
        recv_count => 0,
    } => $class;

    my $data = $self->{data} = _rpc(auth => login => $user, password => md5_hex($pass));

    die "Error: $data->{auth}\n" if $data->{auth};

    my $chatter = $data->{chat_url};
    my $port = 11031;

    say "Got chat server address $chatter:$port";
    say "Connecting to chat server at $chatter";

    POE::Component::Client::TCP->new(
        RemoteAddress => $chatter,
        RemotePort    => $port,
        # Prevent input buffering
        Filter        => "POE::Filter::Stream",
        Disconnected  => sub { say "disconnected"; },
        Connected     => sub { $_[KERNEL]->yield(connected    => @_[ARG0 .. $#_]) },
        ServerInput   => sub { $_[KERNEL]->yield(server_input => @_[ARG0 .. $#_]) },
        #ServerFlushed => sub { say "server flushed"; },
        ServerError   => sub { say "server error"; },
        ObjectStates  => [
            $self => [ qw(
                _check_login_response
                _process_message
                _dispatch

                add_buddy
                channel_presence
                channel_traffic
                check_user
                connected
                ignore_message
                join_channel
                keepalive
                leave_channel
                login_success
                my_presence
                server_input
                whois_response
            ) ],
        ],
    );

    return $self;
}

############################ U T I L I T Y   S U B S ###########################

# TODO convert to an event ?
sub _rpc
{
    my ($method, @args) = @_;
    my $response = $ua->post($cr_url, {
        f => $method,
        @args,
    });

    my $data = lex($response->content);
    delete $data->{0}; # I don't understand this extra top-level key
    return wantarray ? %$data : $data;
}

# TODO replace with a regular handler for 0x00 command code
sub _check_login_response
{
    my ($self, $kernel, $data) = @_[OBJECT, KERNEL, ARG0];
    if (length($data) != 5 || $data ne pack "H*", "0100000000") {
        die "Unexpected login response packet";
    }
}

{ my %cache; # cached names
sub _nick2id
{
    my $i = 0;
    my @need = grep !$cache{$_}, @_;
    my @have = grep  $cache{$_}, @_;
    my %results = mesh @have, @{[ @cache{@have} ]};
    if (@need) {
        %results = (
            %results,
            _rpc(nick2id => map { "nickname[" . $i++ . "]" => $_ } @need)
        );
    }

    return \%results;
}
}

{ my $seq;
# I'm not sure what the purpose of this sequence number is, or even if it is
# a sequence number, but it appears to be a monotonically increasing
# non-time-linear sequence that is used at least for buddy management. We fake
# one.
sub _seq_num
{
    $seq ||= time - 1234567890;
    return $seq += 2;
}
}

# TODO move newly-minted events to their own section, away from these so-called
# "utility subs"
# eats a message from a string and calls the passed sub on it
sub _process_message
{
    my ($self, $kernel, $msgref, $code) = @_[OBJECT, KERNEL, ARG0, ARG1];

    die "Login failed" if $self->{recv_count} > 1 && !$self->{logged_in};

    my ($len) = unpack "V", $$msgref;
    my $this = substr($$msgref, 4, $len);
    $$msgref = substr($$msgref, $len + 4);

    $kernel->yield($code => $this);
}

sub my_presence
{
    say "chat connection confirmed";
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my ($code, $count) = unpack "C V", $message;
    for my $i (0 .. $count - 1) {
        my ($id, $flags) = unpack "V v", substr($message, 5 + ($i * 6), 6);
        printf "    id %u, flags %#x\n", $id, $flags;
    }
}

sub channel_presence
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my $pos = 1; # skip code
    my ($channel, $chanid, undef, $welcome, $specials) = unpack "Z* V C Z* V", substr($message, 1);
    $pos += length($channel) + 1 + 4 + 1 + length($welcome) + 1 + 4;
    say "I ($user) am in channel '$channel'";
    say "    special user(s) registered in this channel ($specials):";

    for my $i (0 .. $specials - 1) {
        my ($id, $flags) = unpack "V C",
                                substr($message, $pos, 5);
        printf "        special user id %u has flags %#x\n", $id, $flags;
        $pos += 5;
    }

    my ($count) = unpack "V", substr($message, $pos, 4);
    $pos += 4;
    say "    there are $count users in channel";

    for my $i (0 .. $count - 1) {
        my ($name, $id, $flags) = unpack "Z* V v",
                                    substr($message, $pos);
        printf "        name=%-16s, id=%7d, flags=0x%04x\n", $name, $id, $flags;
        $pos += length($name) + 1 + 4 + 2;
    }
}

sub channel_traffic
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my ($userid, $chanid, $saying) = unpack "x V V Z*", $message;
    say "user id $userid said in channel id $chanid : '$saying'";
}

sub whois_response
{
    say "whois reponse";
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my $pos = 0;
    my ($code, $name) = unpack "C Z*", $message;
    $pos += 1 + length($name) + 1;
    if ($code == 0x2D) {
        say "user $name is in these channels:";
        my ($count) = unpack "V", substr($message, $pos, 4);
        $pos += 4;
        for my $i (0 .. $count - 1) {
            my ($chan) = unpack "Z*", substr($message, $pos);
            say "    '$chan'";
            $pos += length($chan) + 1;
        }
    }
}

sub ignore_message
{
    #printf "ignoring message code %#02x\n", ord shift;
}

sub _dispatch
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my $key = ord $message;
    if (my $code = $actions{$key}) {
        $kernel->yield($code, $message);
    } else {
        print HexDump($message);
    }
}

sub login_success
{
    my ($self, $kernel) = @_[OBJECT, KERNEL];
    say "login successful";
    $self->{logged_in} = 1;
}

sub connected
{
    my ($self, $kernel, $heap) = @_[OBJECT, KERNEL, HEAP];
    say "authenticating";
    my $auth = pack "C V Z* V", 0xff, $self->{data}{account_id}, $self->{data}{cookie}, 2;
    ($self->{tcp} ||= $heap->{server})->put($auth);
}

sub server_input
{
    my ($self, $kernel, $input) = @_[OBJECT, KERNEL, ARG0];
    say "packet received ($self->{recv_count})";

    if ($self->{recv_count}++ == 0) {
        $kernel->delay(keepalive => $keepalive_period);
        $kernel->yield(join_channel => $home_channel);
    }

    while (length($input) > 0) {
        $kernel->call($_[SESSION], _process_message => \$input, '_dispatch');
        die $! if $!;
    }
}

############################## P O E   E V E N T S #############################

sub check_user
{
    my ($self, $kernel, $user) = @_[OBJECT, KERNEL, ARG0];
    my $server = $self->{tcp};

    say "checking user $user";

    $server->put("*$user\0");
}

sub keepalive
{
    my ($self, $kernel) = @_[OBJECT, KERNEL];
    say "keepalive";
    $self->{tcp}->put(chr 2);
    $kernel->delay(keepalive => $keepalive_period);
}

sub add_buddy
{
    my ($self, $kernel, $buddy) = @_[OBJECT, KERNEL, ARG0];
    my $server = $self->{tcp};

    say "adding buddy $buddy";

    my $id = _nick2id($buddy)->{$buddy};
    my $seq = _seq_num();
    # YAUBS
    my $packed = pack "C V V V", 0x0d, $id, $seq, $seq + 1;
    warn HexDump($packed);
    # TODO
    $server->put($packed);
}

sub join_channel
{
    my ($self, $kernel, $chan) = @_[OBJECT, KERNEL, ARG0];
    say "joining channel '$chan'";
    $self->{tcp}->put(pack "C Z*", 0x1E, $chan);
}

sub leave_channel
{
    my ($self, $kernel, $chan) = @_[OBJECT, KERNEL, ARG0];
    say "leaving channel '$chan'";
    $self->{tcp}->put(pack "C Z*", 0x22, $chan);
}

