---
title: "PostgreSQL & MongoDB Internals: Inside the Storage Engine"
description: "Trace an INSERT and SELECT through PostgreSQL's heap+WAL and MongoDB's WiredTiger — step by step. Storage engines explained with real schema and zero hand-waving."
tags: [database, postgres, mongodb, performance]
series: "Database Internals: From Theory to Benchmarks"
canonical_url:
cover_image:
published: false
---

Post 2 gave you the data structures: B+Tree, B-Tree, LSM Tree - how they're shaped and the tradeoffs they do.

This post traces an `INSERT` and a `SELECT` through both PostgreSQL and MongoDB, step by step, using the same `orders` schema throughout.  

---

## PostgreSQL

### The architecture in one paragraph

Before tracing any queries, map the terrain. PostgreSQL has four major components you'll encounter on every operation:

> ```
> ┌───────────────────────────────────────────────────────────┐
> │                    PostgreSQL Process                     │
> │                                                           │
> │  ┌──────────────────┐          ┌────────────────────────┐ │
> │  │  shared_buffers  │          │   WAL (write-ahead log)│ │
> │  │  (buffer pool)   │          │   sequential log file  │ │
> │  │  8KB pages       │          │   fsynced per commit   │ │
> │  └────────┬─────────┘          └──────────┬─────────────┘ │
> │           │                               │               │
> │  ┌────────▼─────────────────────────────┐ │               │
> │  │              Disk                     │ │               │
> │  │  ┌───────────────┐  ┌──────────────┐  │ │               │
> │  │  │  Heap file    │  │  Index files │  │ │               │
> │  │  │  (rows, ~8KB  │  │  (B+Tree,    │  │ │               │
> │  │  │   pages)      │  │   8KB pages) │  │ │               │
> │  │  └───────────────┘  └──────────────┘  │ │               │
> │  └───────────────────────────────────────┘ │               │
> └─────────────────────────────────────────────────────────┘
> ```

**shared_buffers** is PostgreSQL's in-memory page cache; every read and write touches it first. The **WAL** is an append-only sequential log on disk; it's how PostgreSQL survives crashes. The **heap file** is where rows actually live, in 8KB pages, in roughly insertion order. The **index files** are separate B+Tree structures, also stored as 8KB pages, pointing into the heap. Writes go to the WAL first, then into shared_buffers, and eventually to the heap and index files on disk. Reads check shared_buffers first; if the page isn't there, it comes from disk.

---

### The write path: `INSERT INTO orders`
```sql
INSERT INTO orders
VALUES ('a1b2...', 'u9x8...', 'shipped', 149.99, 'Order for...', now());
```

**1. Parsing and planning**

PostgreSQL parses the SQL into an AST, resolves table and column names against the catalog, and produces a trivial plan: "insert one row into the orders heap, update the primary index on `order_id`, update secondary indexes on `user_id` and `created_at`." No interesting planning happens for a simple insert; the executor takes over immediately.

**2. The WAL write happens before anything else**

Before PostgreSQL touches shared_buffers, before it modifies a single heap page, it writes a WAL record describing this insert. The WAL record contains enough information to reconstruct the change: which relation, which page, what was written.

Why first? Because disk writes are not atomic. If PostgreSQL wrote to the heap file and then crashed before finishing, you'd have a partially written page with no way to know what it should contain. The WAL is written sequentially. On crash, PostgreSQL replays WAL to reconstruct any changes that didn't make it to the heap.

The WAL write is fsynced to disk before the transaction is acknowledged to your application. That fsync is real I/O and it is one of the biggest contributors to PostgreSQL write latency.

This behavior is controlled by `synchronous_commit`. The default (`on`) fsyncs the WAL before acknowledging. Setting it to `off` lets PostgreSQL acknowledge before the fsync, reducing write latency significantly, but accepting up to `wal_writer_delay` (default 200ms) of potential data loss on a hard crash. In the benchmark in Post 5, you'll see exactly how much latency this setting saves. The difference is substantial.

**3. The row lands in shared_buffers**

PostgreSQL now needs a heap page with enough free space for the new row. The `orders` table with a 500-character `description` field has rows of roughly 600–700 bytes. An 8KB page holds about 10–12 of these rows.

PostgreSQL consults the **Free Space Map (FSM)** , a structure that tracks how much free space exists in each heap page, to find a suitable page. It loads that page into shared_buffers if it isn't already there, and writes the new row into the page's free space. The page is now **dirty**; its in-memory version differs from what's on disk. It will be flushed to the heap file eventually, by the checkpointer background process.

The heap is unordered by design. PostgreSQL doesn't store rows sorted by `order_id` or any other key. New rows go wherever there's space. This is what enables fast inserts; you never need to find a sorted position in the heap. The tradeoff is that reads by non-indexed fields require a full sequential scan.

**So what's durable right now:** The WAL record is on disk. If the server crashes at this exact moment, PostgreSQL will replay the WAL on restart and re-apply this insert. The heap page is only in shared_buffers. But that's fine, because the WAL has it covered.

**4. The primary B+Tree index update on `order_id`**

PostgreSQL now updates the `order_id` index. It traverses the B+Tree from the root page to find the leaf page where the new UUID belongs.

For a table with 1 million rows, the B+Tree is typically 3–4 levels deep. Each level is a page read. So if the page is in shared_buffers, it's a memory access. If not, it's a disk read. Root and upper internal pages stay hot in shared_buffers because they're accessed on every operation; leaf pages are the cold part.

UUID keys are random. They don't arrive in sorted order, so each new key lands at a random position in the leaf level. Because of this, **any leaf page can be the target of any insert** and any leaf page can be full. Page splits happen frequently with UUID primary keys. When a leaf page is full, it splits: half its entries move to a new sibling page, and the parent node gets a new routing key. This is two page writes instead of one, plus a parent modification. Under high insert load with UUID keys, this is a real source of write amplification and p99 latency spikes.

Sequential or time-ordered keys (monotonically increasing integers) avoid this almost entirely. Splits only happen at the rightmost leaf as the tree grows forward. If write performance matters for your schema, key selection matters.

**5. Secondary index updates on `user_id` and `created_at`**

Each secondary index is a separate B+Tree on disk. After the heap write, PostgreSQL updates both of them. The `user_id` index leaf entries contain `(user_id_value, ctid)`, the indexed value plus a **tuple ID** pointing to the physical location of the row in the heap (page number + slot number). The `created_at` index works identically.

This means a single `INSERT` into `orders` touches: 1 heap page + 1 primary index leaf page + 1 `user_id` index leaf page + 1 `created_at` index leaf page + WAL records for all of them. That's the minimum. Page splits add more.

This is why more secondary indexes means slower writes. Each index is another B+Tree traversal, another page modification, another WAL record. The cost is O(k) in the number of indexes.

**What's durable right now:** WAL records for all modifications have been fsynced. All four sets of page changes are in shared_buffers, not yet on disk in the heap and index files. None of that matters for durability; the WAL has everything. The checkpointer will flush the pages to disk in the background, at which point the corresponding WAL segments become eligible for recycling.

---

### The read path: primary key and secondary index

#### Scenario A: `SELECT * FROM orders WHERE order_id = <some order id>`

**1. Planning**

The query planner sees a filter on `order_id`, which is the primary key. It knows there's a B+Tree index on this column. For an equality predicate on an indexed column with high selectivity (one specific UUID), an index scan is the obvious choice.

**2. B+Tree traversal**

PostgreSQL starts at the root page of the `order_id` index. Lets say with 1 million rows and 8KB pages, the tree is about 3–4 levels deep. At each level, it reads the node and follows the pointer toward the target UUID. This takes 3–4 page reads to reach the leaf.

Each page read checks shared_buffers first. Root and upper internal pages are almost always warm; they're tiny (a few pages) and accessed constantly. Leaf pages may or may not be cached depending on your workload and how recently this specific range was accessed.

**3. The heap fetch**

The leaf node entry contains a **ctid**, a physical pointer to a specific page and slot in the heap file. PostgreSQL takes that ctid and fetches the heap page. This is a second disk access (or shared_buffers hit) beyond the index traversal.

This two-step structure - index lookup to get a pointer, then heap fetch to get the actual row is the fundamental cost. And it's worth understanding clearly: even the primary index doesn't contain the row data. Rows live in the heap. Indexes are always pointers into the heap.

**4. MVCC - finding the right version**

When PostgreSQL finds the row in the heap page, it may find multiple versions of the same logical row. This is MVCC (Multi-Version Concurrency Control). Every row version has two hidden fields: `xmin` (the transaction ID that created this version) and `xmax` (the transaction ID that deleted or superseded it, or 0 if still live).

PostgreSQL checks these against your transaction's snapshot; the set of transaction IDs that were committed when your query started. If `xmin` is committed and visible to your snapshot, and `xmax` is 0 or not yet committed, this is your row. If another transaction is currently updating this row, you'll find its old version without blocking so MVCC means readers never wait for writers.

**Warm vs cold read:** If the heap page is in shared_buffers, the whole operation takes microseconds. If it's not, then a cold read - PostgreSQL reads it from disk, which is where that ~1.2ms p99 in the benchmark comes from. The OS page cache may have it buffered below the database level, which is faster than physical disk but slower than shared_buffers.

---

#### Scenario B: `SELECT * FROM orders WHERE user_id = $1`

**1. Planning and index choice**

`user_id` is not the primary key — it's a secondary index column. The planner estimates how many rows match this `user_id` value. If it's highly selective (one user with a few orders out of a million), an index scan on the `user_id` B+Tree is the right call. If it's low selectivity (a user with 50,000 orders), a sequential scan of the heap might actually be faster because random heap fetches at scale are slower than a sequential read.

One flag the planner uses is `random_page_cost` — the estimated cost of a random page read relative to a sequential read. The default is 4.0, which reflects spinning disk characteristics. On SSDs, it should be closer to 1.1–1.5. If `random_page_cost` is set too high for your hardware, the planner over-penalizes index scans and may choose a sequential scan when an index scan would be faster. This is a common tuning issue on SSD-backed databases.

**2. Secondary index traversal + heap fetches**

PostgreSQL traverses the `user_id` B+Tree to find all leaf entries matching the target UUID. Each matching entry contains a ctid. For a user with 20 orders, that's 20 ctids. PostgreSQL then fetches each corresponding heap page.

The critical issue: those 20 heap pages are likely scattered randomly across the heap file, because rows were inserted in time order, not user order. That's 20 potentially non-sequential disk reads. This is why secondary index reads are more expensive than primary key reads at scale. Not because the index traversal is slower, but because the heap fetches are random.

PostgreSQL has an optimization called a **bitmap index scan** for this case: it collects all matching ctids first, sorts them by physical page order, then fetches heap pages in order. This converts random reads into something closer to sequential reads. The planner chooses this strategy automatically when it estimates enough matching rows to make the sort worthwhile.

---

### Crash recovery in summary

When PostgreSQL restarts after a crash, it reads the control file to find the position of the last successful **checkpoint**; a moment when all dirty shared_buffers pages were flushed to the heap and index files. From that position forward, PostgreSQL replays every WAL record, re-applying all changes that happened after the checkpoint. Any transaction with a WAL commit record is replayed to completion; any transaction without a commit record is effectively rolled back. When replay finishes, the heap and index files are in a consistent state and the database opens for connections.

---

### The PostgreSQL surprise: dead tuples

Here's something that surprises most engineers when they first encounter it: **PostgreSQL never updates a row in place.**

When you execute `UPDATE orders SET status = 'delivered' WHERE order_id = 1234`, PostgreSQL does not find the existing row and modify it. It writes a **new version** of the row into the heap (in a free slot on the same or a different page), sets `xmax` on the old version to the current transaction ID, and leaves the old version in the heap page. The old version is now a **dead tuple**,  invisible to future transactions but still occupying space.

The heap page now contains more data than it represents. Over time, on a table with frequent updates, heap pages can be mostly dead tuples. This is called **heap bloat**. It wastes disk space and, more importantly, causes reads to do more I/O to find live rows.

**VACUUM** is the background process that reclaims dead tuples. It scans heap pages, identifies tuples whose `xmax` is old enough that no active transaction could ever need them, and marks their space as reusable. `autovacuum` runs this automatically, but under heavy update load it can fall behind.

The reason PostgreSQL does writing new versions rather than modifying in place is MVCC. Concurrent readers may need the old version of a row while a writer is updating it. Both versions need to coexist in the heap until no reader needs the old one. The dead tuple overhead is the cost of non-blocking reads.

Cassandra has a different but equivalent cost: it also writes new versions and marks deletions with tombstones, and the cleanup (compaction) is also asynchronous.

---