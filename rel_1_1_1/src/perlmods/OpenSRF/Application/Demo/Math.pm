package OpenSRF::Application::Demo::Math;
use base qw/OpenSRF::Application/;
use OpenSRF::Application;
use OpenSRF::Utils::Logger qw/:level/;
use OpenSRF::DomainObject::oilsResponse;
#use OpenSRF::DomainObject::oilsPrimitive;
use OpenSRF::EX qw/:try/;
use strict;
use warnings;


sub DESTROY{}

our $log = 'OpenSRF::Utils::Logger';

sub send_request {
	my $self = shift;
	my $client = shift;

	my $method_name = shift;
	my @params = @_;

	my $session = OpenSRF::AppSession->create( "opensrf.dbmath" );
	my $request = $session->request( "dbmath.$method_name", @params );
	my $response = $request->recv();
	if(!$response) { return undef; }
	if($response->isa("Error")) {throw $response ($response->stringify);}
	$session->finish();

	return $response->content;

}
__PACKAGE__->register_method( method => 'send_request', api_name => '_send_request' );

__PACKAGE__->register_method( method => 'add_1', api_name => 'add' );
sub add_1 {
	my $self = shift;
	my $client = shift;
	my @args = @_;

	my $meth = $self->method_lookup('_send_request');
	my ($result) = $meth->run('add',@args);

	return $result;
	
	return send_request( "add", @args );
}

__PACKAGE__->register_method( method => 'sub_1', api_name => 'sub' );
sub sub_1 {
	my $self = shift;
	my $client = shift;
	my @args = @_;

	my $meth = $self->method_lookup('_send_request');
	my ($result) = $meth->run('sub',@args);

	return $result;
	
	return send_request( "sub", @args );
}

__PACKAGE__->register_method( method => 'mult_1', api_name => 'mult' );
sub mult_1 {
	my $self = shift;
	my $client = shift;
	my @args = @_;

	my $meth = $self->method_lookup('_send_request');
	my ($result) = $meth->run('mult',@args);

	return $result;
	
	return send_request( "mult", @args );
}

__PACKAGE__->register_method( method => 'div_1', api_name => 'div' );
sub div_1 {
	my $self = shift;
	my $client = shift;
	my @args = @_;

	my $meth = $self->method_lookup('_send_request');
	my ($result) = $meth->run('div',@args);

	return $result;
	
	return send_request( "div", @args );
}


1;
