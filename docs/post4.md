---
title: "MongoDB Internals: Inside the Storage Engine"
description: "Trace an insertOne or insertMany and find through MongoDB's WiredTiger — step by step. Storage engines explained with real schema and zero hand-waving."
tags: [database, postgres, mongodb, performance]
series: "Database Internals: From Theory to Benchmarks"
canonical_url:
cover_image:
published: false
---

Post 3 explained the flow of INSERT and SELECT from PostgreSQL lense. Now its time for `insertOne`/`insertMany` and `find`.

---

## MongoDB

### How MongoDB is different before you start

Three major differences from PostgreSQL that we will visit in this section.

First, **WiredTiger is a separate, pluggable storage engine** underneath MongoDB. PostgreSQL's storage is tightly integrated with the query engine. WiredTiger is a standalone embeddable key-value store that MongoDB sits on top of. This matters because WiredTiger has its own caching, its own journal, its own compression, and its own concurrency model, somewhat independent of MongoDB's query layer.

Second, **documents are stored as BSON** — a binary encoding where field names are stored as strings inside every document, on disk, for every document in the collection. PostgreSQL's heap rows contain only values; column names live once in the catalog. BSON's field name overhead matters at scale.

Third, MongoDB provides **document-level concurrency**, implemented using WiredTiger’s **optimistic concurrency control and fine-grained locking**, not page-level locking. Two concurrent writes to different documents in the `orders` collection never block each other, even if they land on the same internal storage page. PostgreSQL's page-level LWLocks can cause contention between concurrent writers targeting the same page.

---

### The write path: `insertOne` into orders
```js
db.orders.insertOne({
  order_id: "a1b2...",
  user_id: "u9x8...",
  status: "shipped",
  amount: 149.99,
  description: "Order for...",
  created_at: new Date()
})
```

**1. BSON serialization**

Before anything reaches WiredTiger, the document is serialized to BSON. In BSON, each field is encoded as: a type byte, the field name as a null-terminated string, then the value. For our `orders` document with six fields, the field names themselves (`order_id`, `user_id`, `status`, `amount`, `description`, `created_at`) add roughly 50–70 bytes of overhead per document.

That overhead exists for every document in the collection. For 1 million orders, that's 50–70MB of field name data that PostgreSQL simply doesn't have, because PostgreSQL stores column names once in `pg_attribute`. For a collection with short values and many fields, BSON overhead is a meaningful fraction of total storage. For a collection dominated by large field values (like the 500-character `description`), it's a smaller percentage but never zero.

This is a fundamental consequence of the schema-free document model that the schema travels with the data.

**2. WiredTiger cache - the document lands here first**

WiredTiger maintains its own in-memory cache (configured via `wiredTigerCacheSizeGB`). This is conceptually similar to PostgreSQL's shared_buffers - a pool of in-memory pages that buffer both reads and writes.

The key difference: **WiredTiger stores data compressed on disk but uncompressed in cache**. When a document is written to the WiredTiger cache, it lives there uncompressed. When it's evicted to disk (during a checkpoint), WiredTiger compresses it using Snappy by default. When it's read back from disk (a cold read), it's decompressed as it loads into cache.

This means your configured cache size represents uncompressed data, while your disk usage reflects compressed data. A 4GB WiredTiger cache might correspond to 8–12GB of data on disk, depending on compression ratio. Cold reads pay a decompression cost that PostgreSQL doesn't have in its default configuration.

**3. Journal write - durability before acknowledgment**

WiredTiger has its own journal, conceptually equivalent to PostgreSQL's WAL. It's a sequential, append-only log that describes changes before they're applied to data files.

The key behavioral difference from PostgreSQL is in the default durability setting. PostgreSQL's default `synchronous_commit = on` fsyncs the WAL before every commit. WiredTiger's default journal sync interval is **100 milliseconds**. Acknowledgment can happen before the journal is fsynced, accepting up to 100ms of potential data loss in a hard crash.

MongoDB exposes this to the application as **write concern**. With `j: false`, MongoDB acknowledges the write as soon as WiredTiger's cache accepts it. With `j: true`, MongoDB waits for the journal to be fsynced before acknowledging. The latency difference between these two settings is measurable; `j: true` adds the cost of a synchronous disk flush to every write, similar to what PostgreSQL pays by default.

For the benchmark in Post 6, write concern settings will be explicitly called out because they significantly affect the numbers.

**4. Primary B-Tree index update on `order_id`**

WiredTiger updates the `order_id` B-Tree index. The traversal is the same pattern as PostgreSQL's B+Tree - root to internal nodes to leaf, finding the right position for the new UUID. The same random-UUID page-split problem applies: UUIDs land at random positions, any leaf can be full, splits are frequent.

One architectural distinction worth noting: in MongoDB, the **collection storage itself is a WiredTiger B-Tree keyed by `order_id`**. There isn't a separate heap file and a separate primary index. The collection B-Tree serves as both. The document data lives in the B-Tree's leaf nodes. This is different from PostgreSQL, where the heap is unordered storage and the primary index is a separate B+Tree pointing into it.

**5. Secondary index updates on `user_id` and `created_at`**

MongoDB secondary indexes differ from PostgreSQL's in one important design choice: **MongoDB secondary index entries contain the document's `order_id` value, not a physical storage location**.

In PostgreSQL, a secondary index leaf entry holds a `ctid`- a literal page number and slot number. This is a direct physical pointer into the heap. It's fast to follow at read time, but it becomes stale if the row moves (which can happen during certain heap operations).

MongoDB chose to store `order_id` in secondary index entries instead. The lookup then requires a second step: use the `order_id` to look up the document in the collection's primary B-Tree. This is effectively two B-Tree traversals for a secondary index read.

The reason for this choice: documents in WiredTiger can move within storage during compaction and internal page restructuring. If secondary indexes contained physical locations, every document move would require updating every secondary index entry pointing to it which is potentially expensive. Storing `order_id` instead means document moves never invalidate secondary index entries. The tradeoff is paid at read time with the double traversal.

**6. Document-level locking**

When two concurrent inserts arrive, WiredTiger acquires a document-level lock for each, not a page-level lock. If the two documents happen to land on the same internal B-Tree page, they still don't block each other. So WiredTiger's concurrency model ensures independent documents can be written independently even when they share a storage page.

PostgreSQL's LWLocks are page-level: if two concurrent inserts target the same B+Tree leaf page, one must wait for the other to release the lock before proceeding. Under high concurrent write load to the same key range (like sequential timestamps), this becomes measurable contention.

---

### The read path: primary and secondary key

#### Scenario A: `db.orders.findOne({ order_id: "a1b2..." })`

**1. B-Tree traversal**

MongoDB traverses the `order_id` B-Tree from root to the leaf containing the target UUID. Same depth as PostgreSQL for the same data volume - 3–4 levels for 1M documents. The query planner identifies this as a primary key lookup and routes it through the `order_id` index without deliberation.

MongoDB caches which index to use per **query shape**, the structure of the filter, sort, and projection without the specific values. If you've run `findOne({ order_id: ... })` before, the planner uses the cached plan. PostgreSQL re-evaluates cost-based plans per query but also uses plan caching for prepared statements; MongoDB's trial-based plan caching behaves differently under data distribution changes.

**2. Document fetch and decompression**

Because the collection data lives in the `order_id` B-Tree itself (not a separate heap), the document is retrieved directly from the leaf node. There's no separate heap fetch step. The B-Tree traversal ends with the document.

If the page is in the WiredTiger cache, the document is already uncompressed in memory. If not, a cold read - WiredTiger reads the compressed page from disk, decompresses it into cache, and returns the document. The decompression step adds CPU work to cold reads that PostgreSQL's default configuration doesn't have.

For the benchmark's cold read numbers, this decompression cost is part of why MongoDB's p99 is slightly higher than PostgreSQL's - the disk I/O is similar, but MongoDB adds decompression.

---

#### Scenario B: `db.orders.find({ user_id: "u9x8..." })`

**1. Secondary index traversal**

MongoDB traverses the `user_id` secondary index B-Tree to find all entries matching the target UUID. Each matching leaf entry contains `(user_id_value, _order_id_value)`.

**2. Document lookups by `order_id`**

For each matching `order_id`, MongoDB performs a second B-Tree traversal but this time on the collection's `order_id` B-Tree to retrieve the full document. If a user has 20 orders, that's 20 secondary index traversals + 20 `order_id` lookups. Each `order_id` lookup is a full tree traversal from root to leaf.

This double-traversal is the same fundamental cost as PostgreSQL's secondary index + heap fetch, but the mechanism differs. PostgreSQL follows a ctid (direct physical pointer, one disk seek). MongoDB follows an `order_id` (logical key, full tree traversal). The logical key approach is more robust to storage reorganization; the physical pointer approach is faster per lookup.

Both databases pay the secondary-index penalty. The performance gap between primary key reads and secondary index reads is visible in both engines.

---

### The MongoDB surprise: WiredTiger cache and compression

Engineers who come from PostgreSQL often assume that "cache size" and "data size" are in the same units. In WiredTiger, they're not.

WiredTiger compresses data on disk (Snappy by default achieves 2–4× compression on typical BSON documents). The WiredTiger cache holds data **uncompressed**. This means:

- **The WiredTiger cache holds data uncompressed, but disk stores it compressed.** This means the cache must be sized for the *uncompressed* working set which is 2–4× larger than the on-disk footprint. A working set that occupies 4GB compressed on disk expands to 8–16GB in the WiredTiger cache. Under-sizing the cache relative to this uncompressed working set is a common MongoDB performance issue.
- **Every cold read involves a decompression step.** Reading from disk means reading compressed bytes, then spending CPU cycles to decompress them into cache. On modern hardware this is fast. Snappy decompresses at multiple GB/sec but it's not free, and it doesn't exist in PostgreSQL's default storage.
- **Cache pressure is measured in uncompressed bytes.** To hold 8GB of compressed on-disk data in the WiredTiger cache, you need roughly 8GB × compression_ratio so somewhere between 16GB and 32GB of cache. More cache is required than the raw disk size suggests, not less. This is the inverse of what most engineers assume when first sizing a MongoDB deployment.

The compression tradeoff is generally positive. You get more effective cache and less disk I/O. But it changes the cost model for cold reads in a way that's easy to overlook when sizing hardware.

---

## Direct comparison

After tracing both engines, here's where they converge and where they diverge:

### Where the first write lands
Both PostgreSQL (WAL) and MongoDB (journal) write to a sequential log before touching data structures. The principle is identical - write-ahead logging for crash safety. The difference is the default sync behavior: PostgreSQL fsyncs per commit by default; WiredTiger's journal syncs every 100ms by default. PostgreSQL's default is safer; MongoDB's default is faster.

### Locking granularity
PostgreSQL uses page-level LWLocks - concurrent writes to the same B+Tree leaf page serialize. WiredTiger uses document-level locks - concurrent writes to different documents never block each other. Under high concurrent write load to overlapping key ranges, WiredTiger's finer granularity shows up as better throughput.

### Secondary index design
PostgreSQL secondary indexes store ctids - physical heap pointers. Fast to follow, but tied to physical location. MongoDB secondary indexes store `order_id` - logical keys. Requires a second B-Tree traversal to fetch the document, but immune to document moves. Both choices are deliberate; both have real costs at read time.

### Storage overhead
PostgreSQL stores column names once in the system catalog; heap rows contain only values. MongoDB embeds field names in every BSON document on disk. For our `orders` collection with six fields and 1 million documents, BSON overhead adds roughly 50–70MB that PostgreSQL doesn't have. For collections with larger values, this is a smaller percentage; for collections with many small fields, it's significant.

### Where each engine is faster
PostgreSQL's linked B+Tree leaf nodes make range scans faster - follow the list, read sequentially. MongoDB's document-level locking makes high-concurrency writes more scalable. PostgreSQL's direct ctid heap fetch is faster per secondary index lookup than MongoDB's double B-Tree traversal. WiredTiger's compression means more data fits in cache per GB of RAM.

---

## What's next: when B-Tree assumptions break down

Both PostgreSQL and MongoDB are built on B-Tree variants. They organize data in sorted pages, they update those pages in place, they use WAL or journal for crash recovery, and they handle MVCC by keeping old versions around for concurrent readers. The details differ, but the fundamental bias is the same: **optimize for reads at the cost of write complexity and in-place update overhead**.

Next introduces Cassandra, which starts from the opposite premise. Its storage is append-only and immutable. There are no pages to split, no tuples to vacuum, no in-place updates. Writes are always sequential appends to a log and an in-memory buffer. Reads are more complex because data may be spread across multiple immutable files.

Every performance characteristic that differs between Cassandra and the two engines you just traced flows from that single architectural inversion.

---