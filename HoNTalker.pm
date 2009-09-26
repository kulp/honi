#!/usr/bin/env perl
use strict;
use warnings;
use feature 'say';

# TODO add event forwarding

package HoNTalker;

use Data::HexDump;
use Digest::MD5 qw(md5_hex);
use IO::Socket::INET;
use List::MoreUtils qw(mesh uniq);
use LWP::UserAgent;
use YAML qw(Dump);

use POE qw(Component::Client::TCP Filter::Stream);

use lib ".";
use LexerWrapper qw(lex);

################################################################################

my $base = qq(http://masterserver.hon.s2games.com);
my $cr_url = qq($base/client_requester.php);

################################################################################

my %actions = (
    0x00 => 'login_success',
    0x03 => 'channel_traffic',
    0x04 => 'channel_presence',
    0x05 => 'channel_join_notice',
    0x06 => 'channel_part_notice',
    0x08 => 'received_whisper',
    0x0B => 'my_presence',
    0x20 => 'received_whisper_all',
    0x2D => 'whois_response',

#    0x05 => 'ignore_message',
#    0x06 => 'ignore_message',
#    0x0C => 'ignore_message',
#    0x18 => 'ignore_message',
);

######################### P O E   E N T R Y   P O I N T ########################

unless (caller) {
    my ($user, $pass, $chan) = @ARGV;
    $user and $pass or die "Supply username and password";

    my $client = HoNTalker->new(
            user     => $user,
            password => $pass,
            abide    => [ $chan || () ],
        );
    POE::Kernel->run();
    exit;
}

################################# M E T H O D S ################################

sub new
{
    my ($class, %args) = @_;
    #my $external_handler = delete $args{event_obj};
    my $self = bless {
        abide      => { map { lc $_ => 1 } @{ delete $args{abide} || [] } },
        keepalive  => delete $args{keepalive} || 30,  # seconds
        recv_count => 0,
        ua         => LWP::UserAgent->new,
        %args,
    } => $class;

    my $data = $self->{data} = $self->_rpc(auth =>
            login    => $self->{user},
            password => md5_hex($self->{password}),
        );

    die "Error: $data->{auth}\n" if $data->{auth};

    my $chatter = $data->{chat_url};
    my $port = 11031;

    say "got chat server address $chatter";
    say "connecting to chat server at $chatter:$port";

    POE::Component::Client::TCP->new(
        RemoteAddress => $chatter,
        RemotePort    => $port,
        # Prevent input buffering
        Filter        => "POE::Filter::Stream",
        Disconnected  => sub { die "disconnected"; },
        Connected     => sub { $_[KERNEL]->yield(connected    => @_[ARG0 .. $#_]) },
        ServerInput   => sub { $_[KERNEL]->yield(server_input => @_[ARG0 .. $#_]) },
        #ServerFlushed => sub { say "server flushed"; },
        ServerError   => sub { say "server error"; },
        ObjectStates  => [
            #($external_handler ? ($external_handler => [ qw(
            #    _default
            #) ]) : ()),
            @{ $self->{object_states} || [] },
            $self => $self->{actions} = [ uniq qw(
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
                login_success
                my_presence
                part_channel
                say_in_channel
                server_input
                whois_response
                whisper_user
                whisper_all_friends
            ), values %actions ],
        ],
    );

    return $self;
}

sub actions
{
    return @{ shift->{actions} };
}

sub _nick2id
{
    my $self = shift;
    my $cache = $self->{nick2id};

    my $i = 0;
    my @need = grep !$cache->{$_}, @_;
    my @have = grep  $cache->{$_}, @_;
    my %results = mesh @have, @{[ @$cache{@have} ]};
    if (@need) {
        %results = (
            %results,
            $self->_rpc(nick2id => map { "nickname[" . $i++ . "]" => $_ } @need)
        );
    }

    # TODO use Contextual::Return here
    return \%results;
}

# I'm not sure what the purpose of this sequence number is, or even if it is
# a sequence number, but it appears to be a monotonically increasing
# non-time-linear sequence that is used at least for buddy management. We fake
# one.
sub _seq_num
{
    my $self = shift;
    $self->{seq} ||= time - 1234567890;
    return $self->{seq} += 2;
}

################################## E V E N T S #################################

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

    $self->{chan2id}{$channel} = $chanid;
    $self->{id2chan}{$chanid}  = $channel;

    $pos += length($channel) + 1 + 4 + 1 + length($welcome) + 1 + 4;
    say "I ($self->{user}) am in channel '$channel'";
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

        $self->{nick2id}{$name} = $id;
        $self->{id2nick}{$id}   = $name;

        printf "        name=%-16s, id=%7d, flags=0x%04x\n", $name, $id, $flags;
        $pos += length($name) + 1 + 4 + 2;
        #$kernel->yield(h2i_user_join_channel => $name, $channel);
        $kernel->post($self->{bridge}, h2i_user_join_channel => $name, $channel);
    }

    #$kernel->yield(part_channel => $channel) unless $self->{abide}{lc $channel}
    $kernel->delay(part_channel => 10, $channel) unless $self->{abide}{lc $channel}
}

sub channel_traffic
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my ($userid, $chanid, $saying) = unpack "x V V Z*", $message;
    my $who   = $self->{id2nick}{$userid} || "id $userid";
    my $where = $self->{id2chan}{$chanid} || "id $chanid";
    say "user $who said in channel $where : '$saying'";
    $kernel->post($self->{bridge}, h2i_user_said_in_channel => $who, $where, $saying);
}

sub received_whisper
{
    my ($self, $kernel, $data) = @_[OBJECT, KERNEL, ARG0];
    my ($code, $speaker, $message) = unpack "C Z* Z*", $data;
    say "user $speaker whispered to me: $message";
    $kernel->post($self->{bridge}, h2i_user_whispered_to_me => $speaker, $message);
}

sub received_whisper_all
{
    my ($self, $kernel, $data) = @_[OBJECT, KERNEL, ARG0];
    my ($code, $speaker, $message) = unpack "C Z* Z*", $data;
    say "user $speaker whispered to all his friends: $message";
    $kernel->post($self->{bridge}, h2i_user_whispered_to_friends => $speaker, $message);
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
    #say "packet received ($self->{recv_count})";

    if ($self->{recv_count}++ == 0) {
        $kernel->delay(keepalive => $self->{keepalive});
        $kernel->yield(join_channel => $_) for keys %{ $self->{abide} };
    }

    while (length($input) > 0) {
        $kernel->call($_[SESSION], _process_message => \$input, '_dispatch');
        die $! if $!;
    }
}

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
    $kernel->delay(keepalive => $self->{keepalive});
}

sub add_buddy
{
    my ($self, $kernel, $buddy) = @_[OBJECT, KERNEL, ARG0];
    my $server = $self->{tcp};

    say "adding buddy $buddy";

    my $id = $self->_nick2id($buddy)->{$buddy};
    my $seq = $self->_seq_num();
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
    $self->{abide}{lc $chan} = 1;
    $self->{tcp}->put(pack "C Z*", 0x1E, $chan);
}

sub part_channel
{
    my ($self, $kernel, $chan) = @_[OBJECT, KERNEL, ARG0];
    say "leaving channel '$chan'";
    $self->{tcp}->put(pack "C Z*", 0x22, $chan);
}

sub channel_join_notice
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my ($code, $user, $userid, $chanid, $flags) = unpack "C Z* V V v", $message;
    #$kernel->yield(hon_joined_channel => $userid, $chanid);
    my $chan = $self->{id2chan}{$chanid};
    $kernel->post($self->{bridge}, h2i_user_join_channel => $user, $chan);

    say "User $user joined channel $chan";
}

sub channel_part_notice
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    my ($code, $userid, $chanid) = unpack "C V V", $message;
    #$kernel->yield(hon_parted_channel => $userid, $chanid);

    my $user = $self->{id2nick}{$userid} || "id $userid";
    say "User $user left channel $self->{id2chan}{$chanid}";
    my $chan = $self->{id2chan}{$chanid};
    $kernel->post($self->{bridge}, h2i_user_part_channel => $user, $chan);
}

sub say_in_channel
{
    my ($self, $kernel, $chan, $message) = @_[OBJECT, KERNEL, ARG0, ARG1];
    say "looking up chanid for $chan";
    my $chanid = $self->{chan2id}{$chan} || return;
    say "putting message $message to chanid $chanid";
    $self->{tcp}->put(pack "C Z* V", 0x03, $message, $chanid);
}

sub whisper_user
{
    my ($self, $kernel, $user, $message) = @_[OBJECT, KERNEL, ARG0, ARG1];
    say "whispering message $message to user $user";
    $self->{tcp}->put(pack "C Z* Z*", 0x08, $user, $message);
}

sub whisper_all_friends
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    say "whispering message $message to all friends";
    $self->{tcp}->put(pack "C Z*", 0x20, $message);
}

############################ U T I L I T Y   S U B S ###########################

# TODO convert to an event ?
sub _rpc
{
    my ($self, $method, @args) = @_;
    my $response = $self->{ua}->post($cr_url, {
        f => $method,
        @args,
    });

    my $data = lex($response->content);
    delete $data->{0}; # I don't understand this extra top-level key
    return wantarray ? %$data : $data;
}

1;

