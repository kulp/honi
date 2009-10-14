#!/usr/bin/env perl
use strict;
use warnings;
use feature 'say';

package HoN2IRC;

#use Data::HexDump;
#use Digest::MD5 qw(md5_hex);
#use IO::Socket::INET;
#use List::MoreUtils qw(mesh);
#use LWP::UserAgent;
#use YAML qw(Dump);

use POE qw(
    Component::Server::IRC
    Filter::Stream
    );

#use lib ".";

our $my_name = q(_h2i);
our $FRIENDS_CHANNEL = q(&friends);
our $CONTROL_CHANNEL = q(&control);
our $CMD_PREFIX = q(!);

######################### P O E   E N T R Y   P O I N T ########################

#unless (caller) {
#    my $client = HoN2IRC->new();
#    POE::Kernel->run();
#    exit;
#}

################################# M E T H O D S ################################

sub new
{
    my ($class, %args) = @_;
    my $self = bless {
        %args,
    } => $class;

    my $ircd = $self->{ircd} = POE::Component::Server::IRC->spawn(
            servername => 'hon2irc.kulp.ch',
            network    => 'HoN2IRCNet',
            nicklen    => 20,
            antiflood  => 0,    # debugging only
            #auth       => 0,    # debugging only
        );

#    POE::Session->create(
#            object_states => [
#                @{ $self->{object_states} || [] },
#                $self => [ qw(
#                    _default
#                    ircd_daemon_join
#                    ircd_daemon_part
#                    ircd_registered
#                ) ] ],
#        );

    return $self;
}

sub _is_me
{
    my ($self, $arg) = @_;
    $arg =~ /^[~@]?\b$self->{user}\b/;
}

sub ircd_registered
{
    my ($self, $kernel) = @_[OBJECT, KERNEL];

    $self->{ircd}->add_listener(port => 8889);
    $self->{ircd}->add_operator({
            username => $self->{user},
            password => $self->{password},
        });
    $self->{ircd}->yield(add_spoofed_nick => {
            nick    => $my_name,
            umode   => 'Boi',
            ircname => __PACKAGE__." bot",
        });
    $self->{ircd}->yield(daemon_cmd_join => $my_name => '&status');
}

sub ircd_daemon_join
{
    my ($self, $kernel, $user, $chan) = @_[OBJECT, KERNEL, ARG0, ARG1];
    my $nick = _nick($user);
    return if $nick eq $my_name;
    _unsafen(my $clean = $chan);
    $self->{ircd}->yield(daemon_cmd_join => $my_name => $chan);
    return if $chan =~ /^[+&]/; # special channels, don't translate to HoN channels
    # TODO rename event name (put a prefix on it)
    # TODO stop joining the channel over and over again if we are just getting
    # notification of our own spoofed join
    $kernel->yield(join_channel => $clean);
}

sub ircd_daemon_public
{
    my ($self, $kernel, $nick, $chan, $message) = @_[OBJECT, KERNEL, ARG0, ARG1, ARG2];
    my $name = _name($nick);
    # keep a map of safename <=> realname for maps (and for nicks too ?)
    say "$name said >>$message<< in $chan";
    if ($self->_is_me($name)) {
        if ($chan eq $FRIENDS_CHANNEL) {
            $kernel->yield(whisper_all_friends => $message);
        } elsif ($chan eq $CONTROL_CHANNEL) {
            $kernel->yield(dispatch_command => $message);
        } else {
            # TODO check that I am in this channel
            $kernel->yield(say_in_channel => _safen($chan), $message);
        }
    }
}

sub ircd_daemon_privmsg
{
    my ($self, $kernel, $nick, $user, $message) = @_[OBJECT, KERNEL, ARG0, ARG1, ARG2];
    my $name = _name($nick);
    say "$name said >>$message<< to $user";
    # it shouldn't be possible for this check to be false, but we want
    # to be sure we don't start a loop
    if ($self->_is_me($name)) {
        # TODO map the user back to a safename
        $kernel->yield(whisper_user => $user, $message);
    }
}

sub ircd_daemon_part
{
    my ($self, $kernel, $user, $chan) = @_[OBJECT, KERNEL, ARG0, ARG1];
    my $nick = _nick($user);
    return if $nick eq $my_name;
    $self->{ircd}->yield(daemon_cmd_part => $my_name => $chan);
    if ($self->_is_me($user)) {
        $kernel->yield(part_channel => _unsafen(my $clean = $chan));
    }
}

sub h2i_user_part_channel
{
    my ($self, $kernel, $user, $chan) = @_[OBJECT, KERNEL, ARG0, ARG1];
    my $safechan = $chan; _safen($safechan);
    my $safeuser = $user; _safen($safeuser);
    # TODO check that user is valid before parting
    #$self->{ircd}->yield(daemon_cmd_part => $safeuser => "#$safechan");
    say "DEBUG: user $user ($safeuser) parted channel $chan ($safechan)";
}

sub h2i_user_join_channel
{
    my ($self, $kernel, $user, $chan) = @_[OBJECT, KERNEL, ARG0, ARG1];
    # TODO can user names be unsafe ?
    my $safechan = $chan; _safen($safechan);
    my $safeuser = $user; _safen($safeuser);
    $self->{ircd}->yield(add_spoofed_nick => {
            nick    => $safeuser,
            # TODO make the mode mean something
            #umode   => 'v',
            ircname => $user,
        });
    $self->{ircd}->yield(daemon_cmd_join => $safeuser => "#$safechan");
    $self->{ircd}->yield(daemon_cmd_join => $safeuser => '&known');
    say "DEBUG: user $user ($safeuser) joined channel $chan ($safechan)";
}

sub h2i_user_said_in_channel
{
    my ($self, $kernel, $user, $chan, $message) = @_[OBJECT, KERNEL, ARG0, ARG1, ARG2];
    # TODO can user names be unsafe ?
    my $safechan = $chan; _safen($safechan);
    my $safeuser = $user; _safen($safeuser);
    $self->{ircd}->yield(daemon_cmd_privmsg => $safeuser, "#$safechan", $message);
}

sub h2i_user_whispered_to_me
{
    my ($self, $kernel, $user, $message) = @_[OBJECT, KERNEL, ARG0, ARG1];
    my $safeuser = $user; _safen($safeuser);
    # TODO make this idempotent ? wrap it ?
    $self->{ircd}->yield(add_spoofed_nick => {
            nick    => $safeuser,
            # TODO make the mode mean something
            #umode   => 'v',
            ircname => $user,
        });
    $self->{ircd}->yield(daemon_cmd_privmsg => $safeuser, $self->{user}, $message);
}

sub h2i_user_whispered_to_friends
{
    # TODO differentiate from regular whisper
    my ($self, $kernel, $user, $message) = @_[OBJECT, KERNEL, ARG0, ARG1];
    my $safeuser = $user; _safen($safeuser);
    $self->{ircd}->yield(add_spoofed_nick => {
            nick    => $safeuser,
            # TODO make the mode mean something
            #umode   => 'v',
            ircname => $user,
        });
    $self->{ircd}->yield(daemon_cmd_privmsg => $safeuser, $self->{user}, $message);
}

sub h2i_general_notice
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    $self->{ircd}->yield(daemon_cmd_notice => $my_name, $self->{user}, $message);
}

sub dispatch_command
{
    my ($self, $kernel, $message) = @_[OBJECT, KERNEL, ARG0];
    if (my ($cmd, $args) = $message =~ /^\Q$CMD_PREFIX\E (\w+) (?:\s+ (.*?))? \s*$/x) {
        my @args = split " ", $args;
        # TODO implement commands table
    }
}

# Taken from the POE::Component::Server::IRC documentation. For debugging only.
sub _default
{
    my ( $event, $args ) = @_[ ARG0 .. $#_ ];
    print STDOUT "$event: ";
    foreach (@$args) {
        SWITCH: {
            if ( ref($_) eq 'ARRAY' ) {
                print STDOUT "[", join ( ", ", @$_ ), "] ";
                last SWITCH;
            }
            if ( ref($_) eq 'HASH' ) {
                print STDOUT "{", join ( ", ", %$_ ), "} ";
                last SWITCH;
            }
            print STDOUT "'$_' ";
        }
    }

    print STDOUT "\n";
    return 1;    # Don't handle signals.
}

sub _nick    { (split /!/, $_[0], 2)[0] }
sub _name    { (split /!/, $_[0], 2)[1] }
# Make functional, not mutative
sub _unsafen { $_[0] =~ y/_&#+/ /d; $_[0] }
sub _safen   { $_[0] =~ y/ &#+/_/d; $_[0] }

1;

