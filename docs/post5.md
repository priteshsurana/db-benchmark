---
title: "Cassandra Internals: LSM Tree, SSTables, and Compaction"
description: "How Cassandra's LSM engine works end to end - CommitLog, Memtable, SSTable, Bloom filters, and compaction. Why writes are fast and reads are complex before cleanup runs."
tags: [database, cassandra, backend, performance]
series: "Database Internals: From Theory to Benchmarks"
canonical_url:
cover_image:
published: false
---

Post 3 and 4 traced writes and reads through PostgreSQL and MongoDB. Both engines use B-Tree variants. Both optimize for reads - maintaining sorted indexes, linking leaf nodes, storing heap pointers or link to primary index and pay for that optimization with write complexity: page splits, locking, dead tuples, in-place update overhead.

Cassandra makes the opposite bet. It never modifies anything on disk. Every write is an append. Every file, once written, is immutable until compaction removes it. The read path pays for this. It has to reconcile data across potentially many files to find the latest version of a row. Understanding Cassandra means understanding why that tradeoff is worth making for certain workloads, and how the engine manages the read cost through Bloom filters and compaction.

---

## Why Two Tables instead of One

Before any internals, the schema needs explaining. As this is a continuation in the series, read previous parts to understand the Orders table schema and how we might have to change it if using Cassandra. Here it is again:
```sql
TABLE: orders_by_id
  PRIMARY KEY (order_id)

TABLE: orders_by_user
  PRIMARY KEY (user_id, created_at)
```

In PostgreSQL, you'd have one `orders` table with a secondary index on `user_id`. The engine maintains that index; you write a row once, and PostgreSQL handles the index update. In Cassandra, you write the data **twice**, in two different shapes, into two different tables.

This isn't a design quirk. It follows directly from how LSM storage works.

LSM's write strength is sequential appends to known partition keys. Given a partition key, Cassandra can write to the right Memtable instantly - no page to find, no B-Tree to traverse, no lock to acquire. But if you want to query by a different field than the partition key, you need a different table with that field as the partition key. Cassandra does have secondary indexes, but they're implemented as hidden tables under the hood and carry significant read cost - scanning across SSTables that weren't organized for that access pattern. For production workloads, the idiomatic solution is: **write the data multiple times, once per access pattern**.

The consequence is that every INSERT into `orders` triggers two writes:
```plaintext
INSERT event:
  → write to orders_by_id   (keyed by order_id)
  → write to orders_by_user (keyed by user_id + created_at)
```

This is write amplification - paying more on the write side to make reads efficient. The tradeoff is explicit and application-owned. PostgreSQL maintains its secondary indexes for you, transparently. Cassandra requires you to maintain your denormalized tables explicitly. If `orders_by_user` gets out of sync with `orders_by_id`, that's your problem, not the engine's.

Why accept this burden? Because at high write throughput - millions of inserts per minute - Cassandra's append-only writes stay fast under load in a way that B-Tree engines struggle to match. The write amplification is a known, bounded cost. If not, then the alternative - secondary indexes on an LSM engine under heavy write load - is an unbounded performance hazard.

---

## The Architecture

Five components handle most of the things in Cassandra's LSM engine:
```plaintext
Write path:
  Client INSERT
       │
       ▼
  ┌──────────────┐     sequential append, fast
  │  CommitLog   │ ──────────────────────────── disk
  └──────────────┘
       │
       ▼
  ┌──────────────┐     sorted in-memory buffer
  │   Memtable   │ ──────────────────────────── RAM
  └──────────────┘
       │  (when full)
       ▼
  ┌──────────────────────────────────────────┐
  │  SSTable (immutable sorted file)          │
  │  + Bloom filter (probabilistic index)     │
  │  + Partition index (byte offsets)         │
  └──────────────────────────────────────────┘ disk

Background:
  ┌──────────────┐
  │  Compaction  │  merges SSTables, resolves versions, removes tombstones
  └──────────────┘
```

The **CommitLog** is the crash-safety log - write here first, before touching any data structure. The **Memtable** is the in-memory sorted buffer that accumulates writes. When the Memtable fills, it flushes to disk as an **SSTable** - an immutable, sorted, self-contained file. Each SSTable has a **Bloom filter** (to quickly rule out keys that aren't in that file) and a **partition index** (to locate specific keys within the file). **Compaction** periodically merges SSTables, consolidating versions and reclaiming space from tombstones.

---

## The Write Path
```sql
INSERT INTO orders_by_id
  (order_id, user_id, status, amount, description, created_at)
VALUES (uuid(), uuid(), 'shipped', 149.99, 'Order for...', toTimestamp(now()));
```

**1. Partition key hashing**

Cassandra hashes the `order_id` value using Murmur3 to produce a **token** - a number in a very large range that determines where this row lives in the cluster's token space. On a single node, every token maps to the same node, so routing is trivial. In a multi-node cluster (Post 7), this hash determines which node receives the write. No coordinator needs a lookup table - any node can compute the owner from the hash.

**2. CommitLog append**

Before the row touches the Memtable, Cassandra appends a record to the **CommitLog** - a sequential, append-only file on disk. This is Cassandra's equivalent of PostgreSQL's WAL, but structurally simpler. There are no page boundaries, no page headers, no B-Tree node structures. It's a flat sequence of mutation records, written front to back. Cassandra also compresses CommitLog segments, reducing the I/O cost relative to PostgreSQL's uncompressed WAL writes.

The CommitLog exists purely for crash recovery. If Cassandra crashes before the Memtable is flushed to an SSTable, the CommitLog lets it reconstruct the lost Memtable on restart.

Cassandra offers two CommitLog sync modes:
- **Periodic** (default): the CommitLog is synced to disk every ~10 seconds. Writes are acknowledged before the sync. Up to 10 seconds of data can be lost in a hard crash. But is fast.
- **Batch**: the CommitLog is synced before every acknowledgment. No data loss window. Slower. Every write pays a disk flush, similar to PostgreSQL's default `synchronous_commit = on`.

For the benchmark in Post 6, the sync mode is called out explicitly because it significantly affects write throughput numbers.

**3. Memtable write - the client is acknowledged here**

After the CommitLog append, the row is written to the **Memtable** for `orders_by_id`. The Memtable is a sorted in-memory data structure sorted by partition key, then by clustering key within each partition. For `orders_by_id` with only a partition key, the sort is by `order_id`. For `orders_by_user` with `(user_id, created_at)`, rows are sorted first by `user_id`, then by `created_at` within each user.

Once the Memtable write is complete, **Cassandra acknowledges the write to the client**. No disk access to a data file. No B-Tree traversal. No page split. No locking against other writers. The write touched a sequential log file and an in-memory structure. That's it. This is why Cassandra's single-threaded insert throughput in the Post 1 benchmark was ~18,000 rows/sec compared to PostgreSQL's ~8,500.

**4. The second Memtable write — write amplification in practice**

The application now sends the second INSERT for the same order event:
```sql
INSERT INTO orders_by_user
  (user_id, created_at, order_id, status, amount)
VALUES (uuid(), toTimestamp(now()), uuid(), 'shipped', 149.99);
```

This goes through the same CommitLog + Memtable path, but to the `orders_by_user` Memtable. Two CommitLog appends, two Memtable writes, two eventual SSTable entries for one logical business event. The write amplification is real. It's the cost of Cassandra's access pattern design, and it's visible in disk space usage and write throughput measurements on multi-table schemas.

**5. Memtable flush --> the SSTable is born**

When the Memtable reaches its size threshold (configurable, typically 256MB–1GB), Cassandra flushes it to disk as a new **SSTable**. The flush is a single sequential write pass from beginning to end. As the Memtable is already sorted, so no sort step is needed. Cassandra writes the entire sorted buffer to a new file in one pass. Sequential. Fast. The best possible disk write pattern.

The resulting SSTable is **immutable**. It will never be modified after being written. If a row is later updated, the update goes to a new Memtable and eventually a new SSTable. The old SSTable keeps its old version.

Three files are written alongside the SSTable data file:

- **Bloom filter**: a compact probabilistic structure that, given a partition key, can definitively answer "this key is NOT in this SSTable" (no false negatives). It answers "maybe yes" for keys that are there, and occasionally for keys that aren't (false positives). Kept in memory. Eliminates most unnecessary SSTable reads.
- **Partition index**: maps each partition key in this SSTable to its byte offset in the data file. Used to seek directly to a partition without reading the whole file.
- **Partition summary**: a sparse sample of the partition index, kept in memory. Used to narrow down the range to read from the partition index itself, avoiding a full index scan.

Once the SSTable is written and fsynced, the CommitLog segments that covered those writes are eligible for deletion as the data is now safe in the SSTable.

**What's durable at each step:**

| Moment | What's safe |
|--------|-------------|
| After CommitLog append | The write survives a crash, replay reconstructs the Memtable |
| After Memtable write | Same guarantee, Memtable is in RAM, CommitLog is the safety net |
| After SSTable flush | Doubly safe, SSTable on disk, CommitLog segment now disposable |
| After compaction | Cleaned up, old versions and tombstones removed, read path faster |

---

## Updates and Deletes: The Immutability Consequence

Since SSTables are never modified, Cassandra cannot update or delete a row the way PostgreSQL does.

**Updates** write a new version. If you update `status` from `'shipped'` to `'delivered'` on `order_id = X`, Cassandra writes a new row to the current Memtable with `status = 'delivered'` and a newer timestamp. The old row with `status = 'shipped'` still exists in an older SSTable. Before compaction runs, **both versions are on disk**. Reads resolve this by comparing timestamps and newest wins.

**Deletes** write a **tombstone**, a special record that marks a partition key (or specific row or column) as deleted at a particular timestamp. The tombstone goes to the Memtable and eventually an SSTable. The original data still sits in its original SSTable. Before compaction, the data is still there on disk; reads see the tombstone, find that it's newer than the data, and return nothing.

This means a table that has seen many updates looks like this on disk:
```plaintext
SSTable-1 (oldest):
  order X: status=shipped,   ts=1000
  order Y: status=pending,   ts=1001

SSTable-3 (newer):
  order X: status=delivered, ts=1500  ← newer version
  order Z: [tombstone]       ts=1600  ← delete marker

SSTable-5 (newest):
  order Y: status=shipped,   ts=2000  ← another update
```

The disk footprint of an `orders` table with a busy update pattern is larger than the logical size of the data. Every historical version of every row exists in some SSTable until compaction removes it. Monitoring SSTable count and disk amplification is part of running Cassandra in production.

Compare this to PostgreSQL's dead tuples: PostgreSQL also keeps old row versions around (in heap pages) and cleans them with VACUUM. Different mechanism, same root cause - both engines must keep old versions available for concurrent readers or crash recovery, and both accumulate waste that a background process cleans up. The specifics differ, but neither is free.

---

## The Read Path

### Scenario A: `SELECT * FROM orders_by_id WHERE order_id = ?`

**1. Check the Memtable**

The read path starts in memory. Is the target `order_id` in the current Memtable? If yes, return that version. It's the newest possible. If no, continue to SSTables.

**2. Bloom filter check for each SSTable**

Cassandra checks the Bloom filter for every SSTable on disk. A Bloom filter check is a memory operation - the filters are loaded into RAM. For each SSTable, the result is one of:
- **Definitely not here**: skip this SSTable entirely. No disk read.
- **Maybe here**: proceed to check the partition index.

With 20 SSTables and a Bloom filter false positive rate of ~1%, most SSTables are eliminated with zero disk I/O. The few that pass the filter get a partition index lookup.

**3. Partition index lookup**

For each SSTable that passes its Bloom filter, Cassandra checks the **partition summary** (in memory) to narrow the range, then reads the relevant portion of the **partition index** from disk to find the exact byte offset of this `order_id` in the SSTable data file.

**4. Read the partition from the SSTable**

Cassandra reads the partition data from the byte offset identified in step 3. This is the actual disk read.

**5. Merge versions across SSTables**

If the key appeared in multiple SSTables, Cassandra now has multiple versions of rows or cells with different timestamps. It merges them: for each column, the version with the highest timestamp wins. Tombstones suppress any data with an older timestamp. The result is the most recent consistent version of the row.

**What read amplification looks like in practice:**
```plaintext
After 1 flush (1 SSTable):
  → 1 Bloom filter check
  → 1 partition index lookup
  → 1 data read
  → No merge needed
  ≈ fast

After 20 flushes (20 SSTables), before compaction:
  → 20 Bloom filter checks (memory, fast)
  → ~1-3 partition index lookups (most filtered by Bloom)
  → 1-3 data reads (disk)
  → Merge step across matched versions
  ≈ slower, and the p99 tail grows with SSTable count
```

This is the compaction effect Post 6's benchmark will put exact numbers on: the same cold read query, the same hardware, the same data - and a p99 latency that drops by more than 7× once SSTable count falls from 8 to 1. The engine cleaned up after itself, and reads got proportionally faster.

---

### Scenario B: `SELECT * FROM orders_by_user WHERE user_id = ?`

This query goes to the `orders_by_user` table - a completely separate set of SSTables with `user_id` as the partition key. The read path is identical to Scenario A: Memtable check, Bloom filters, partition index, data read, merge.

Here's the thing to notice: **this is a primary key lookup on `orders_by_user`, not a secondary index lookup**. The cost was paid at write time, when the application wrote to both tables. The read is as efficient as any partition key read on any table. There's no equivalent of PostgreSQL's heap fetch, no secondary B-Tree traversal, no ctid resolution step.

This is the core architectural contrast with PostgreSQL:

| Operation | PostgreSQL | Cassandra |
|---|---|---|
| Write one order | 1 heap write + N index updates | 2 table writes (explicit, application-owned) |
| Read by `order_id` | Primary index → heap fetch | Partition lookup on `orders_by_id` |
| Read by `user_id` | Secondary index → heap fetch | Partition lookup on `orders_by_user` |
| Who pays for the secondary access | Engine, at read time | Application, at write time |

PostgreSQL does the secondary access work at read time. The heap fetch and index maintenance are handled transparently. Cassandra moves that cost to write time - you write twice, but both reads are primary lookups. Same total work, different distribution across the write/read boundary.

---

## Compaction: Where the magic happens.

Every post about Cassandra mentions compaction. Most treat it as an operational detail. It's not. Compaction is the corrective force that makes the LSM design sustainable. Without it, SSTables would accumulate indefinitely, reads would get progressively slower, and tombstones would never be reclaimed.

### Why compaction exists

Every Memtable flush produces a new SSTable. Updates and deletes produce additional versions and tombstones in newer SSTables. Over time:
- SSTable count grows → read amplification grows
- Disk space grows → old versions and tombstones consume space that no longer represents live data
- Read latency grows → more files to check, more merging at read time

Compaction is the mechanism that reverses all three.

### What happens during compaction

Cassandra selects a set of SSTables to compact (which ones depends on the strategy). Then:

1. Open all selected SSTables simultaneously and read them in sorted partition key order. Because each SSTable is individually sorted, merging them is a merge sort efficiently.
2. For each partition key, collect all versions and cells from all selected SSTables.
3. For each cell, keep only the version with the highest timestamp.
4. For tombstones: if the tombstone is older than `gc_grace_seconds` (default: 10 days), drop both the tombstone and the data it deletes. If it's newer, keep the tombstone in the output if it may still be needed.
5. Write the merged, deduplicated, tombstone-cleaned result as a new SSTable.
6. Delete the input SSTables.

The output is a single, clean SSTable with no duplicate versions, no stale tombstones, and a fresh Bloom filter and partition index reflecting only live data.

The `gc_grace_seconds` window exists for multi-node safety: in a cluster, a tombstone needs time to propagate to all replicas. If compaction removed a tombstone before all replicas saw it, a replica that missed the deletion could serve the deleted data as if it were live. Ten days is the conservative window to ensure propagation completes.

### The direct effect on reads

After compaction, the SSTable count drops. Fewer SSTables means:
- Fewer Bloom filter checks per read
- Fewer partition index lookups
- Less data to merge
- Shorter, faster read path

This is the benchmark result from Post 1 made concrete. Cassandra read p99 went from ~4.1ms to ~1.4ms after compaction. The engine cleaned up after itself, and reads got proportionally faster.

### The three compaction strategies

**STCS - Size-Tiered Compaction Strategy** (the default)

Groups SSTables by size and merges groups of similarly-sized ones together. Think of it as bins: small SSTables merge into medium ones, medium into large, large into very large. Write-optimized, each byte of data participates in few compaction passes. The downside: at any moment you can have many SSTables at the small tier, which means read amplification spikes under heavy write load before a tier-level merge runs.

Use STCS for write-heavy workloads where compaction I/O budget is limited.

**LCS - Leveled Compaction Strategy**

Organizes SSTables into levels (L0, L1, L2, ...), where each level is 10× larger than the previous. From L1 onward **no two SSTables at the same level overlap in key range**. A read needs to check at most one SSTable per level from L1 up. With 5 levels, that's at most 5 SSTables for any read, regardless of how many total SSTables exist.

The exception is L0. L0 receives Memtable flushes directly and SSTables here *can* have overlapping key ranges, they arrive as flushed, unsorted relative to each other. A read must check every L0 SSTable. This is why L0 SSTable count is the critical operational metric for LCS: under heavy write load, L0 accumulates faster than compaction can promote files to L1, and read amplification rises until the backlog clears. A healthy LCS table keeps L0 small. Typically under 4 files.

Read-optimized above L0. Bounded read amplification once L0 is under control. The cost: compaction is more frequent and more I/O-intensive because every write eventually needs to be organized into non-overlapping ranges at each level.

Use LCS for read-heavy workloads where predictable read latency matters more than compaction overhead.

**TWCS - Time-Window Compaction Strategy**

Divides SSTables into time windows (e.g., one window per day). SSTables within a window are compacted together; windows don't compact across each other. When a window's TTL expires, the entire SSTable for that window is deleted as a unit - no need to read the data, just drop the file.

Built for time-series data with TTL. Extremely efficient for the "write once, read briefly, expire in bulk" pattern. Breaks down for workloads that update historical data, because updates write new timestamps that cross window boundaries.

Use TWCS for time-series tables, event logs, anything with uniform TTL.

### The operational cost

During compaction, both the input SSTables (being read) and the output SSTable (being written) exist on disk simultaneously. Peak disk usage during a compaction can be roughly 2× the size of the data being compacted. Cassandra nodes should never run above ~50% disk utilization, or compaction may fail for lack of space.

Compaction also competes with live reads and writes for disk I/O. Cassandra has throughput throttling for compaction, but under heavy write load that produces SSTables faster than compaction can consume them, read amplification can climb even with throttling. Monitoring SSTable count per table is a core Cassandra operational metric.

---

## The Cassandra Production Footgun: Tombstones

Here's the scenario that causes real production incidents.

Your application deletes all orders with `status = 'cancelled'` from a partition lets say, all orders for a specific user in `orders_by_user`. Each delete writes a tombstone. The data still exists in the older SSTables. For the next `gc_grace_seconds` (10 days by default), **every read of that partition must process every tombstone**.

Now imagine a partition with 100,000 cancelled orders, all tombstoned. A read for that user's current orders must scan through 100,000 tombstones to find the handful of live rows. Even if the application considers those orders "deleted," Cassandra is reading every tombstone at query time. Reads that should return 5 rows in milliseconds take seconds because of tombstone scanning.

Cassandra will warn you that there's a `tombstone_warn_threshold` (default: 1,000 tombstones per read) and a `tombstone_failure_threshold` (default: 100,000). Hitting the failure threshold causes reads to be aborted. Both are real production incident causes at companies running Cassandra at scale.

The mitigation: set TTLs on data instead of deleting it, use TWCS so expired data is dropped as whole SSTables rather than tombstoned row by row, and monitor tombstone metrics actively. The problem doesn't appear in development because development data volumes are too small to trigger it.

---

## Three Engines, One Comparison

Now that you've seen all three storage paths, here's how they line up:

**Where the write lands first**

PostgreSQL and MongoDB both write to a sequential log first (WAL / WiredTiger journal), then modify in-memory page structures (shared_buffers / WiredTiger cache). Cassandra also writes to a sequential log first (CommitLog), then writes to an in-memory sorted buffer (Memtable). The durability pattern is the same - log first, then memory, then data files. The difference is what the in-memory structure is: a page cache holding B-Tree nodes (PG/Mongo) vs a sorted write buffer that will become an immutable file (Cassandra).

**What makes writes fast**

PostgreSQL and MongoDB writes involve finding the right position in a B-Tree, acquiring page or document locks, potentially splitting pages, and writing WAL records for modified pages. Under sustained write load, page splits and locking create latency variance. Cassandra writes append to a log and insert into a sorted RAM buffer. No page to find, no lock to acquire, no split to handle. The write path is maximally simple. The price is paid elsewhere.

**What makes reads complex**

PostgreSQL and MongoDB reads follow a tree to a single authoritative location. The heap or the B-Tree leaf. The data is there, in one place. Cassandra reads must check multiple SSTables, each of which may contain a version of the requested row. Bloom filters eliminate most checks, but the merge step is always present when multiple versions exist. Read complexity grows with SSTable count and shrinks after compaction.

**Who owns the secondary access pattern**

PostgreSQL maintains secondary indexes automatically. MongoDB maintains them automatically. The application writes once and the engine handles multiple access paths. In Cassandra, the application owns the secondary access pattern by writing to multiple tables. This is more work for the application developer and more disk space consumed. But both reads end up as efficient primary key lookups on their respective tables, which is not true of B-Tree secondary indexes that require heap fetches.

---

## What's Next: The Numbers

Posts 2, 3, 4 and 5 have been entirely about understanding *why* each engine behaves the way it does. Post 6 is where that understanding meets measurement.

Real C++ clients. 1 million rows. Identical hardware. Cold reads and warm reads. Write throughput under sustained load. Pre-compaction and post-compaction latency distributions. The full picture.

The most surprising result in the benchmark is not the one you'd predict from the theory alone. You'll need to read coming post to find out what it is.
