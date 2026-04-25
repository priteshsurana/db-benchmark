-- orders_by_id - primary access pattern (parition: order_id)
-- orders_by_user - secondary access pattern (partition: user_id)

-- cqlsh 192.168.56.13 9042 -u cassandra -p cassandra -f init.cql

-- using  SimpleStrategy + RF=1 for single-node benchmark.

CREATE KEYSPACE IF NOT EXISTS benchmark
WITH replication = {
	'classl		: 'SimpleStrategy',
	'replication_factor': 1
}
AND durable_writes = true;

-- the keyspace for all subsequent statements

USE benchmark;

-- paritiion key: order_id (UUID)
-- for compaction: STCS so its size tiered write optimized the default option
-- Groups SSTables of similar size and merges them
-- this will be good for write heavy workloads but also has read amplification before compaction


CREATE TABLE IF NOT EXISTS orders_by_id (
	order_id	UUID,
	user_id		UUID,
	product_id	UUID,
	status		TEXT,
	amount		DECIMAL,
	quantity	SMALLINT,
	description	TEXT,
	created_at	TIMESTAMP,
	updated_at	TIMESTAMP,
	PRIMARY KEY	(order_id)
) WITH
	compaction = {
	'class'		: 'SizeTieredCompactionStrategy',
	'min_threshold'	: 4,
	'max_threshold'	: 32
	}
	AND compression = {
		'sstable_compression'	: 'LZ4Compressor'
	}
	AND comment = 'Primary order lookup table. One partition per order.';



-- orders_by_user
-- fetch all the orders by user
-- sorted by created_at descending i.e. sorted_first/


-- partitioning by user_id
-- with this users orders will basically be colocate so better retieval

CREATE TABLE IF NOT EXISTS orders_by_user (
	user_id		UUID,
	created_at	TIMESTAMP,
	order_id	UUID,
	product_id	UUID,
	status		TEXT,
	amount		DECIMAL,
	quantity	SMALLINT,
	description	TEXT,
	PRIMARY KEY	(user_id, created_at)
) WITH
	CLUSTERING ORDER BY (created_at DESC)
	AND compaction = {
		'class'		: 'SizeTieredCompactionStrategy',
		'min_threshold' : 4,
		'max_threshold'	: 32
	}
	AND compression = {
		'sstable_compression' : 'LZ4Compression'
	}
	AND comment = 'Orders per user, sorted newest first. Enabling secondary access pattern.';



DESCRIBE KEYSPACE benchmark;
