package OpenSRF::Application::Persist;
use base qw/OpenSRF::Application/;
use OpenSRF::Application;

use OpenSRF::Utils::SettingsClient;
use OpenSRF::EX qw/:try/;
use OpenSRF::Utils qw/:common/;
use OpenSRF::Utils::Logger;
use JSON;
use DBI;

use vars qw/$dbh $log $default_expire_time/;

sub initialize {
	$log = 'OpenSRF::Utils::Logger';

	$sc = OpenSRF::Utils::SettingsClient->new;

	my $dbfile = $sc->config_value( apps => 'opensrf.persist' => app_settings => 'dbfile');
	unless ($dbfile) {
		throw OpenSRF::EX::PANIC ("Can't find my dbfile for SQLite!");
	}

	my $init_dbh = DBI->connect("dbi:SQLite:dbname=$dbfile","","");
	$init_dbh->{AutoCommit} = 1;
	$init_dbh->{RaiseError} = 0;

	$init_dbh->do( <<"	SQL" );
		CREATE TABLE storage (
			id	INTEGER PRIMARY KEY,
			name_id	INTEGER,
			value	TEXT
		);
	SQL

	$init_dbh->do( <<"	SQL" );
		CREATE TABLE store_name (
			id	INTEGER PRIMARY KEY,
			name	TEXT UNIQUE
		);
	SQL

	$init_dbh->do( <<"	SQL" );
		CREATE TABLE store_expire (
			id		INTEGER PRIMARY KEY,
			atime		INTEGER,
			expire_interval	INTEGER
		);
	SQL

}

sub child_init {
	my $sc = OpenSRF::Utils::SettingsClient->new;

	$default_expire_time = $sc->config_value( apps => 'opensrf.persist' => app_settings => 'default_expire_time' );
	$default_expire_time ||= 300;

	my $dbfile = $sc->config_value( apps => 'opensrf.persist' => app_settings => 'dbfile');
	unless ($dbfile) {
		throw OpenSRF::EX::PANIC ("Can't find my dbfile for SQLite!");
	}

	$dbh = DBI->connect("dbi:SQLite:dbname=$dbfile","","");
	$dbh->{AutoCommit} = 1;
	$dbh->{RaiseError} = 0;

}

sub create_store {
	my $self = shift;
	my $client = shift;

	my $name = shift || '';

	try {
	
		my $continue = 0;
		try {
			_get_name_id($name);

		} catch Error with { 
			$continue++;
		};

		throw OpenSRF::EX::WARN ("Duplicate key:  object name [$name] already exists!  " . $dbh->errstr)
			unless ($continue);

		my $sth = $dbh->prepare("INSERT INTO store_name (name) VALUES (?);");
		$sth->execute($name);
		$sth->finish;

		unless ($name) {
			my $last_id = $dbh->last_insert_id(undef, undef, 'store_name', 'id');
			$name = 'AUTOGENERATED!!'.$last_id;
			$dbh->do("UPDATE store_name SET name = '$name' WHERE id = '$last_id';");
		}

		_flush_by_name($name);
		return $name;
	} catch Error with {
		return undef;
	};
}
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.slot.create',
	method => 'create_store',
	argc => 1,
);


sub create_expirable_store {
	my $self = shift;
	my $client = shift;
	my $name = shift || do { throw OpenSRF::EX::InvalidArg ("Expirable slots must be given a name!") };
	my $time = shift || $default_expire_time;

	try {
		($name) = $self->method_lookup( 'opensrf.persist.slot.create' )->run( $name );
		return undef unless $name;

		$self->method_lookup('opensrf.persist.slot.set_expire')->run($name, $time);
		return $name;
	} catch Error with {
		return undef;
	};

}
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.slot.create_expirable',
	method => 'create_expirable_store',
	argc => 2,
);

sub _update_expire_atime {
	my $id = shift;
	$dbh->do('UPDATE store_expire SET atime = ? WHERE id = ?', {}, time(), $id);
}

sub set_expire_interval {
	my $self = shift;
	my $client = shift;
	my $slot = shift;
	my $new_interval = shift;

	try {
		my $etime = interval_to_seconds($new_interval);
		my $sid = _get_name_id($slot);

		$dbh->do('DELETE FROM store_expire where id = ?', {}, $sid);
		return 0 if ($etime == 0);

		$dbh->do('INSERT INTO store_expire (id, atime, expire_interval) VALUES (?,?,?);',{},$sid,time(),$etime);
		return $etime;
	} 
}
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.slot.set_expire',
	method => 'set_expire_interval',
	argc => 2,
);

sub find_slot {
	my $self = shift;
	my $client = shift;
	my $slot = shift;

	my $sid = _get_name_id($slot);
	return $slot if ($sid);
	return undef;
}
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.slot.find',
	method => 'find_slot',
	argc => 2,
);

sub get_expire_interval {
	my $self = shift;
	my $client = shift;
	my $slot = shift;

	my $sid = _get_name_id($slot);
	my ($int) = $dbh->selectrow_array('SELECT expire_interval FROM store_expire WHERE id = ?;',{},$sid);
	return undef unless ($int);

	my ($future) = $dbh->selectrow_array('SELECT atime + expire_interval FROM store_expire WHERE id = ?;',{},$sid);
	return $future - time();
}
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.slot.get_expire',
	method => 'get_expire_interval',
	argc => 2,
);


sub _sweep_expired_slots {
	return if (shift());

	my $expired_slots = $dbh->selectcol_arrayref(<<"	SQL", {}, time() );
		SELECT id FROM store_expire WHERE (atime + expire_interval) <= ?;
	SQL

	return unless ($expired_slots);

	$dbh->do('DELETE FROM storage WHERE name_id IN ('.join(',', map { '?' } @$expired_slots).');', {}, @$expired_slots);
	$dbh->do('DELETE FROM store_expire WHERE id IN ('.join(',', map { '?' } @$expired_slots).');', {}, @$expired_slots);
	for my $id (@$expired_slots) {
		_flush_by_name(_get_id_name($id), 1);
	}
}

sub add_item {
	my $self = shift;
	my $client = shift;

	my $name = shift or do {
		throw OpenSRF::EX::WARN ("No name specified!");
	};

	my $value = shift || '';

	try {
		my $name_id = _get_name_id($name);
	
		if ($self->api_name =~ /object/) {
			$dbh->do('DELETE FROM storage WHERE name_id = ?;', {}, $name_id);
		}

		$dbh->do('INSERT INTO storage (name_id,value) VALUES (?,?);', {}, $name_id, JSON->perl2JSON($value));

		_flush_by_name($name);

		return $name;
	} catch Error with {
		return undef;
	};
}
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.object.set',
	method => 'add_item',
	argc => 2,
);
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.queue.push',
	method => 'add_item',
	argc => 2,
);
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.stack.push',
	method => 'add_item',
	argc => 2,
);

sub _get_id_name {
	my $name = shift or do {
		throw OpenSRF::EX::WARN ("No slot id specified!");
	};


	my $name_id = $dbh->selectcol_arrayref("SELECT name FROM store_name WHERE id = ?;", {}, $name);

	if (!ref($name_id) || !defined($name_id->[0])) {
		throw OpenSRF::EX::WARN ("Slot id [$name] does not exist!");
	}

	return $name_id->[0];
}

sub _get_name_id {
	my $name = shift or do {
		throw OpenSRF::EX::WARN ("No slot name specified!");
	};


	my $name_id = $dbh->selectrow_arrayref("SELECT id FROM store_name WHERE name = ?;", {}, $name);

	if (!ref($name_id) || !defined($name_id->[0])) {
		throw OpenSRF::EX::WARN ("Slot name [$name] does not exist!");
	}

	return $name_id->[0];
}

sub destroy_store {
	my $self = shift;
	my $client = shift;

	my $name = shift;

	my $problem = 0;
	try {
		my $name_id = _get_name_id($name);
	
		$dbh->do("DELETE FROM storage WHERE name_id = ?;", {}, $name_id);
		$dbh->do("DELETE FROM store_name WHERE id = ?;", {}, $name_id);
		$dbh->do("DELETE FROM store_expire WHERE id = ?;", {}, $name_id);

		_sweep_expired_slots();
		return $name;
	} catch Error with {
		return undef;
	};

}
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.slot.destroy',
	method => 'destroy_store',
	argc => 1,
);

sub _flush_by_name {
	my $name = shift;
	my $no_sweep = shift;
 
	my $name_id = _get_name_id($name);

	unless ($no_sweep) {
 		_update_expire_atime($name);
		_sweep_expired_slots();
	}
	
	if ($name =~ /^AUTOGENERATED!!/) {
		my $count = $dbh->selectcol_arrayref("SELECT COUNT(*) FROM storage WHERE name_id = ?;", {}, $name_id);
		if (!ref($count) || $$count[0] == 0) {
			$dbh->do("DELETE FROM store_name WHERE name = ?;", {}, $name);
		}
	}
}
	
sub pop_queue {
	my $self = shift;
	my $client = shift;

	my $name = shift or do {
		throw OpenSRF::EX::WARN ("No queue name specified!");
	};

	try {
		my $name_id = _get_name_id($name);

		my $value = $dbh->selectrow_arrayref('SELECT id, value FROM storage WHERE name_id = ? ORDER BY id ASC LIMIT 1;', {}, $name_id);
		$dbh->do('DELETE FROM storage WHERE id = ?;',{}, $value->[0]) unless ($self->api_name =~ /peek$/);

		_flush_by_name($name);

		return JSON->JSON2perl( $value->[1] );
	} catch Error with {
		#my $e = shift;
		#return $e;
		return undef;
	};
}
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.queue.peek',
	method => 'pop_queue',
	argc => 1,
);
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.queue.pop',
	method => 'pop_queue',
	argc => 1,
);


sub peek_slot {
	my $self = shift;
	my $client = shift;

	my $name = shift or do {
		throw OpenSRF::EX::WARN ("No slot name specified!");
	};
	my $name_id = _get_name_id($name);

	my $order = 'ASC';
	$order = 'DESC' if ($self->api_name =~ /stack/o);
	
	my $values = $dbh->selectall_arrayref("SELECT value FROM storage WHERE name_id = ? ORDER BY id $order;", {}, $name_id);

	$client->respond( JSON->JSON2perl( $_->[0] ) ) for (@$values);

	_flush_by_name($name);
	return undef;
}
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.queue.peek.all',
	method => 'peek_slot',
	argc => 1,
	stream => 1,
);
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.stack.peek.all',
	method => 'peek_slot',
	argc => 1,
	stream => 1,
);


sub store_size {
	my $self = shift;
	my $client = shift;

	my $name = shift or do {
		throw OpenSRF::EX::WARN ("No queue name specified!");
	};
	my $name_id = _get_name_id($name);

	my $value = $dbh->selectcol_arrayref('SELECT SUM(LENGTH(value)) FROM storage WHERE name_id = ?;', {}, $name_id);

	return JSON->JSON2perl( $value->[0] );
}
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.queue.size',
	method => 'shift_stack',
	argc => 1,
);
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.stack.size',
	method => 'shift_stack',
	argc => 1,
);
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.object.size',
	method => 'shift_stack',
	argc => 1,
);

sub store_depth {
	my $self = shift;
	my $client = shift;

	my $name = shift or do {
		throw OpenSRF::EX::WARN ("No queue name specified!");
	};
	my $name_id = _get_name_id($name);

	my $value = $dbh->selectcol_arrayref('SELECT COUNT(*) FROM storage WHERE name_id = ?;', {}, $name_id);

	return JSON->JSON2perl( $value->[0] );
}
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.queue.length',
	method => 'shift_stack',
	argc => 1,
);
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.stack.depth',
	method => 'shift_stack',
	argc => 1,
);

sub shift_stack {
	my $self = shift;
	my $client = shift;

	my $name = shift or do {
		throw OpenSRF::EX::WARN ("No slot name specified!");
	};

	try {
		my $name_id = _get_name_id($name);

		my $value = $dbh->selectrow_arrayref('SELECT id, value FROM storage WHERE name_id = ? ORDER BY id DESC LIMIT 1;', {}, $name_id);
		$dbh->do('DELETE FROM storage WHERE id = ?;',{}, $value->[0]) unless ($self->api_name =~ /peek$/);

		_flush_by_name($name);

		return JSON->JSON2perl( $value->[1] );
	} catch Error with {
		my $e = shift;
		return undef;
	};
}
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.stack.peek',
	method => 'shift_stack',
	argc => 1,
);
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.stack.pop',
	method => 'shift_stack',
	argc => 1,
);

sub get_object {
	my $self = shift;
	my $client = shift;

	my $name = shift or do {
		throw OpenSRF::EX::WARN ("No object name specified!");
	};

	try {
		my $name_id = _get_name_id($name);

		my $value = $dbh->selectrow_arrayref('SELECT name_id, value FROM storage WHERE name_id = ? ORDER BY id DESC LIMIT 1;', {}, $name_id);
		$dbh->do('DELETE FROM storage WHERE name_id = ?',{}, $value->[0]) unless ($self->api_name =~ /peek$/);

		_flush_by_name($name);

		return JSON->JSON2perl( $value->[1] );
	} catch Error with {
		return undef;
	};
}
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.object.peek',
	method => 'shift_stack',
	argc => 1,
);
__PACKAGE__->register_method(
	api_name => 'opensrf.persist.object.get',
	method => 'shift_stack',
	argc => 1,
);

1;
