# QuickDC configuration data
# (So we don't need to specify everything by commandline)
package config;

use strict;
use Socket;
1;

sub new {
	my $class = shift;
	my $self = {
		port     => 1411,
		cid      => "",
		pid      => "",
		nick     => "",
		hubaddr  => "",
		ua       => "",
	};

	my $file = "config.conf";

	open(CONFIG, $file) || die "Unable to open configuration file ($file): $!\n";
	my @data = <CONFIG>;
	my $line;
	foreach $line (@data) {
		chomp($line);
		if ($line =~ /^Port\s*=\s*(\d+).*/) {
			$self->{port} = $1;
		}

		if ($line =~ /^PID\s*=\s*(.*)/) {
			$self->{pid} = $1;
		}

		if ($line =~ /^CID\s*=\s*(.*)/) {
			$self->{cid} = $1;
		}


		if ($line =~ /^Nickname\s*=\s*(.*)/) {
			$self->{nick} = $1;
		}

		if ($line =~ /^Hub\s*=\s*(.+)/) {
			$self->{hubaddr} = $1;
		}

		if ($line =~ /^UserAgent\s*=\s*(.*)\s*$/) {
			$self->{ua} = $1;
		}

	}
	close(CONFIG) || die "Unable to close configuration file: $!\n";

	bless $self, $class;
	return $self;
}

sub getLocalPort {
	my $self = shift;
	return $self->{port};
}

sub getNick {
	my $self = shift;
	return $self->{nick};
}

sub getHubAddress {
	my $self = shift;
	return $self->{hubaddr};
}

sub getUserAgent {
	my $self = shift;
	return $self->{ua};
}

sub getPID {
	my $self = shift;
	return $self->{pid};
}

sub getCID {
	my $self = shift;
	return $self->{cid};
}

