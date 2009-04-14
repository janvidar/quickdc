#!/usr/bin/perl -w

package adchub;

use strict;
use adc;
use Socket;
use IO::Handle;
use IO::Socket::INET;
1;

sub new {
	my $class = shift;
	my($host, $port) = @_;
	
	my $self = {
		host     => $host,
		port     => $port,
		sid      => "",
		cid      => "",
		pid      => "",
		nick     => "",
		server   => undef,
		ua       => "Perl ADC Compliance Tester/0.3",
		i4       => "0.0.0.0"
	};

	print "Adchub at " . $self->{host} . ":" . $self->{port} . "\n";

	bless $self; #, $class;
	return $self;
}

sub set_nick
{
	my $self = shift;
	my($str) = @_;
	$self->{nick} = $str;
}

sub set_ua
{
	my $self = shift;
	my ($str) = @_;
	$self->{nick} = $str;
}

sub set_pid
{
	my $self = shift;
	my($str) = @_;
	$self->{pid} = $str;
}

sub set_cid
{
	my $self = shift;
	my($str) = @_;
	$self->{cid} = $str;
}

sub set_i4
{
	my $self = shift;
	my ($str) = @_;
	$self->{i4} = $str;
}


sub connect
{
	my $self = shift;
	$self->{server} = new IO::Socket::INET (
							PeerAddr => $self->{host},
							PeerPort => $self->{port},
							Proto => 'tcp',
							);

	die "$!" unless $self->{server};
	
	$self->{server}->autoflush(1);
}

sub disconnect
{
	my $self = shift;
	close($self->{server});
}


sub recv
{
	my $self = shift;
	return $self->{server}->getline();
}

sub send
{
	my $self = shift;
	my ($line) = @_;
	print "send: $line";
	$self->{server}->send($line);
}



sub parse_sid
{
	my $self = shift;
	my ($line) = @_;
	
	if ($line =~ /^ISID (....)\n$/) {
		if (!&adc::is_valid_base32($1)) {
			die "Error: invalid sid received from hub: '" . $line . "'\n";
		}
		$self->{sid} = $1;
	} else {
		die "Error: invalid sid received from hub: '" . $line . "'\n";
	}

}

sub parse_inf
{
	my $self = shift;
	my ($line) = @_;
	
	if ($line =~ /^(B|I|C)INF (.*)\n$/) {
		# print "IINF\n";
		return 1;
	} elsif ($line =~ /^.STA/) {
		my $code = $self->parse_sta($line);
	} else {
		die "Error: invalid inf received from hub: '" . $line . "'\n";
	}
	return 0;
}

sub parse_sta
{
	my $self = shift;
	my ($line) = @_;
	if ($line =~ /^ISTA (\d\d\d)(.*)?/) {
		return $1;
	}
	return 0;
}

sub send_hsup
{
	my $self = shift;
	$self->send("HSUP ADBASE ADBAS0\n");
}

sub send_binf
{
	my $self = shift;
	$self->send("BINF " . $self->{sid} . " ID" . $self->{cid} . " PD" . $self->{pid} . " NI" . &adc::escape($self->{nick}). " VE" . &adc::escape($self->{ua}) . " SS0 SF0 I4" . $self->{i4} . "\n");
}


