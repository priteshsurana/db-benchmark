-- psql -h localhost -U benchmark -d benchmark -f init.sql

CREATE TABLE IF NOT EXISTS users (
	user_id		UUID		PRIMARY KEY,
	username	VARCHAR(50)	NOT NULL,
	email		VARCHAR(100)	NOT NULL,
	country		CHAR(2)		NOT NULL,
	age		SMALLINT	NOT NULL,
	created_at	TIMESTAMPTZ	NOT NULL
);


CREATE TABLE IF NOT EXISTS orders (
	order_id	UUID		PRIMARY KEY,
	user_ID		UUID		NOT NULL,
	product_id	UUID		NOT NULL,
	status		VARCHAR(20)	NOT NULL,
	amount		NUMERIC(10, 2)	NOT NULL,
	quantity 	SMALLINT	NOT NULL,
	description	TEXT		NOT NULL,
	created_at 	TIMESTAMPTZ	NOT NULL,
	updated_at	TIMESTAMPTZ	NOT NULL
);


-- CREATE INDEX idx_orders_user_id ON orders(user_id);
-- CREATE INDEX idx_orders_created_at ON orders(created_at);

\echo 'Postgre schema  initialized'
\dt
