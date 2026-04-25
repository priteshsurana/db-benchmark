db = db.getSiblingsDB('benchmark');
if (db.getCollectionNames().includes('orders')) {
	print('Dropping existing orders collection ...');
	db.orders.drop();
}

db.createCollection('orders', {
	validator: {
		$jsonSchema: {
			bsonType: 'object',
			required: [
				'_id',
				'user_id',
				'product_id',
				'status',
				'amount',
				'quantity',
				'description',
				'created_at',
				'updated_at'
			],
			properties: {
				_id: {
					bsonType: 'string',
					description: 'UUIDv4 string'
				},
				user_id: {
					bsonTypes: 'string',
					description: 'UUIDv4 string'
				},
				product_id: {
					bsonType: 'string',
					description: 'UUIDv4 string'
				},
				status: {
					bsonTypes: 'sring',
					enum: ['pending', 'shipped', 'delivered', 'cancelled'],
					description: 'Order status'
				},
				amount: {
					bsonTypes: 'double',
					minimum: 0.01,
					description: ''
				},
				quantity: {
					bsonTypes: 'int',
					minimum: 0.01,
					maximum: 10,
					description: '',
				},
				description: {
					bsonTypes: 'string',
					description: 'padding of 600chars',
				},
				created_at: {
					bsonTypes: 'string',
					description: 'ISO UTC timestamp',
				},
				updated_at: {
					bsonTypes: 'string',
					description: 'ISO UTC timestamp'
				}
			}
		}
	},
	validationLevel: 'moderate',
	validationAction: 'warn'
});

print('orders collection cretaed...');
printjson(db.getCollectionNames());
