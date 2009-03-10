#!/usr/bin/perl -w

package adcp2p;

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
		cid      => "",
		token    => "",
		server   => undef,
	};

#	print "Adchub at " . $self->{host} . ":" . $self->{port} . "\n";

	bless $self; #, $class;
	return $self;
}

sub set_token
{
	my $self = shift;
	my($str) = @_;
	$self->{token} = $str;
}

# sub set_pid
# {
#	my $self = shift;
#	my($str) = @_;
#	$self->{pid} = $str;
# }

sub set_cid
{
	my $self = shift;
	my($str) = @_;
	$self->{cid} = $str;
}


sub connect
{
	my $self = shift;
	$self->{server} = new IO::Socket::INET (
							PeerAddr => $self->{host},
							PeerPort => $self->{port},
							Proto => 'tcp',
							);

	# die "$!" unless $self->{server};
	
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

sub parse_inf
{
	my $self = shift;
	my ($line) = @_;
	
	if ($line =~ /^CINF (.*)\n$/) {
		# print "IINF\n";
		return 1;
	} elsif ($line =~ /^.STA/) {
		my $code = $self->parse_sta($line);
	} else {
		die "Error: invalid inf received from client: '" . $line . "'\n";
	}
	return 0;
}

sub parse_sta
{
	my $self = shift;
	my ($line) = @_;
	if ($line =~ /^CSTA (\d\d\d)(.*)?/) {
		return $1;
	}
	return 0;
}

sub send_sup
{
	my $self = shift;
	$self->send("CSUP ADBASE ADBAS0 ADTEST ADBZIP\n");
}

sub send_inf
{
	my $self = shift;
	if ($self->{token} eq "") {
		$self->send("CINF ID" . $self->{cid} . " VE" . &adc::escape("perl test tool") . "\n");
	} else {
		$self->send("CINF ID" . $self->{cid} . " TO" . $self->{token} . " VE" . &adc::escape("perl test tool") . "\n");
	}
	
}

sub send_gfi
{
	my $self = shift;
	my ($type, $file) = @_;
	$self->send("CGFI " . $type . " " . $file . "\n");
}

sub send_get
{
	my $self = shift;
	my ($type, $file, $offset, $length) = @_;
	$self->send("CGET " . $type . " " . $file . " " . $offset . " " . $length . "\n");
}
