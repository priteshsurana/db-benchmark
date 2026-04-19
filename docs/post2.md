---
title: "B+Tree vs LSM Tree: Why Your Database's Data Structure Is Everything"
description: "B+Tree, B-Tree, LSM Tree — the three structures that explain almost every database performance characteristic. Deep dive with visuals, tradeoffs, and real implications."
tags: [database, postgres, cassandra, performance]
series: "Database Internals: From Theory to Benchmarks"
canonical_url:
cover_image:
published: false
---

In [Post 1](https://dev.to/priteshsurana/what-actually-happens-when-you-call-insert-a93), we looked at a benchmark result where Cassandra wrote 2× faster than PostgreSQL — then read 3× slower before compaction ran. Same hardware. Same data. Wildly different numbers in both directions.

That result is a direct consequence of the data structures each engine is built on. Cassandra and PostgreSQL made opposite choices at the foundation level, and those choices ripple through every read, every write, and every latency number you'll ever measure.

This post explains those choices.  

---

## The Problem All Database Indexes Must Solve

Before we look at any specific data structure, let's talk about why this problem is hard.

You want three things from a database index:

1. **Fast writes.** When you insert an order, the index should update quickly.
2. **Fast reads.** When you query by `order_id`, finding the right row should take as few disk operations as possible.
3. **Efficient range scans.** When you query orders between two dates, the engine should be able to find the start of the range and read forward — not scatter-gather across random disk locations.

Every storage engine is making a tradeoff between these goals, and the tradeoff it makes determines its entire performance profile.

Data stored in sorted order is fast to read sequentially and fast to scan in ranges. But keeping data sorted as new writes arrive requires finding the right position for each new key - which means touching existing data structures, reading before you can write, which adds latency. If instead you just append new data without sorting, writes are fast but reads become expensive because you have to search unsorted data.

Every storage engine resolves this tension differently. Let's look at each approach.

---

## B+Tree - The Structure That Powers PostgreSQL

The B+Tree is the dominant data structure in relational databases and has been for decades. PostgreSQL uses it for every index. It's a tree, and to understand it, you need to picture what that tree actually looks like.

### Nodes, leaves, and the shape of the tree

Imagine you're storing `order_id` values (UUIDs) in an index. The B+Tree organizes these into a hierarchy of **nodes**. Each node holds a sorted list of keys and pointers.

There are two kinds of nodes:

**Internal nodes** hold keys and pointers to child nodes. They exist purely for navigation - you use them to route your search toward the right leaf. They don't hold the actual row data.

**Leaf nodes** hold the actual data (or pointers to it). 
In PostgreSQL's case, leaf nodes in a secondary index hold `(key, heap tuple ID)` pairs - the key you indexed, and a pointer to where the actual row lives in the heap file.

Picture the tree shape:
```plaintext
                    [  G  |  P  ]                ← root (internal node)
                   /       |      \
          [B | D | F]   [H | L]   [R | T | W]   ← internal nodes
         /   |   |  \     ...          ...
       [A-B][C-D][E-F][G-H]...                   ← leaf nodes (actual data)
```

The root and internal nodes are small. The leaves are where the bulk of the data lives. For a table with millions of rows, the tree might be only 3-4 levels deep. That's 3-4 node reads to find any row, regardless of table size. This is what makes B+Tree reads fast.

### Leaf nodes form a linked list - and this matters for range scans

Here's the detail that makes B+Trees especially good for range queries: **all leaf nodes are linked together in a doubly linked list**, in sorted key order.
```plaintext
[A-B] ↔ [C-D] ↔ [E-F] ↔ [G-H] ↔ [I-J] → ...
```

Why does this matter? Consider a query like:
```sql
SELECT * FROM orders WHERE created_at BETWEEN '2026-01-01' AND '2026-03-31';
```

The engine traverses the tree from the root to find the leaf containing `2026-01-01`. It reads that leaf. Then, instead of going back to the root to find the next range, it just follows the linked list pointer to the next leaf page, then the next, reading forward sequentially until it passes `2026-03-31`.

Range scans on a B+Tree are essentially sequential reads through a linked list once you've found the starting point. On modern storage, sequential reads are dramatically faster than random reads. This is a core reason B+Trees are the default for databases with complex querying needs.

### Pages — the unit of disk I/O

Each node in the tree maps to a **page** — a fixed-size chunk of data, typically 8KB in PostgreSQL. A page is the smallest unit the database reads from or writes to disk. If you need one row from a leaf node, you read the entire 8KB page that contains it.

This is important for understanding write cost. When you modify a node — say, inserting a new key into a leaf — you read the 8KB page, modify it in memory, and write the full 8KB page back to disk. Even if your change was one row.

### Page splits — the hidden cost of inserts

Here's where B+Tree write performance gets interesting.

Every leaf page has a finite capacity. When a leaf page is full and a new key must be inserted into it, the page must **split**: the existing entries are divided between the old page and a new sibling page, and the parent internal node gets a new key pointing to the new sibling.

Visualize it:
```plaintext
Before split — leaf page is full:
┌─────────────────────────────┐
│  A | C | E | G | J | L | N  │  ← full
└─────────────────────────────┘

Insert "K" → triggers split:

┌───────────────┐    ┌───────────────┐
│  A | C | E | G │    │ J | K | L | N │
└───────────────┘    └───────────────┘
         ↑
  parent node gets new key "J" pointing to right sibling
```

This split writes two pages instead of one, and also modifies the parent node. If the parent is also full, it splits too — and the cascade can propagate up to the root. Root splits are rare but expensive.

For sequential integer keys (1, 2, 3, ...), splits only happen at the rightmost leaf, the tree just grows a new page at the end. Predictable and cheap.

For random UUID keys, each insert lands at a random position in the key space. Any leaf can be the target. Any leaf can be full. **Splits happen frequently and unpredictably.** This is why p99 write latency for UUID primary keys is higher than p50 - most inserts are fast, but the splits that hit full pages cause latency spikes that show up in the tail.

This is also write amplification: one logical insert can cause two or more physical page writes.

### Why B+Tree reads are fast, writes have variance

To summarize the B+Tree:

- **Reads:** 3–4 page reads to find any row in a million-row table. Fast, predictable.
- **Range reads:** Follow the leaf linked list sequentially. Very fast.
- **Writes:** Usually fast, but page splits cause write amplification and latency spikes on random keys. The variance shows up at p99.

---

## B-Tree - What MongoDB Uses and How It Differs

MongoDB's WiredTiger storage engine uses a B-Tree, and most engineers use the terms B-Tree and B+Tree interchangeably. They're not the same - but the difference that matters for WiredTiger isn't the one most textbooks describe.

The key difference is in the leaf layer.

In a **B+Tree**, all leaf nodes are linked together in a doubly linked list in sorted key order. Once you find the start of a range, you follow the chain forward page by page without re-entering the tree. This is what makes PostgreSQL range scans fast: a `BETWEEN` query traverses the tree once to find the starting leaf, then reads forward along the linked list to the end of the range.

In WiredTiger's B-Tree, leaf nodes hold the actual document data, same as a B+Tree so far. The structural difference is that **leaf nodes are not linked**. There is no chain to follow between adjacent leaves. To advance from one leaf to the next during a range scan, the engine must re-enter the tree from a higher level to find the next page.

What this means in practice:

**Range scans are more expensive per step.** A PostgreSQL range scan follows linked leaf pages sequentially. A WiredTiger range scan re-traverses the tree structure to reach each successive page. For small range scans the difference is minimal. For large ones, the linked list wins — which is why direct comparison of benchmark numbers in later post shows PostgreSQL's linked B+Tree leaf nodes as a range-scan advantage over MongoDB.

**Point reads are equivalent.** Both structures find a single key in O(log n) page reads. Neither has a meaningful advantage for typical table sizes.

For most transactional workloads — point lookups, small range scans, mixed reads and writes — the practical performance difference between WiredTiger's B-Tree and PostgreSQL's B+Tree is small. WiredTiger's implementation is heavily optimized and the engine adds its own caching and concurrency mechanisms on top.

The important comparison is not B-Tree vs B+Tree. It's both of them versus what comes next.

---

## LSM Tree - The Structure That Powers Cassandra

The Log Structured Merge Tree (LSM Tree) starts from a completely different premise: what if we never modified data on disk at all?

This is the key insight that makes Cassandra's write throughput possible.

### The core insight: sequential writes beat random writes

On any storage medium — spinning disk, SSD, NVMe — sequential writes are faster than random writes. On a spinning disk, the difference is enormous (the head doesn't need to seek). On an SSD, it's smaller but real (flash write amplification is lower for sequential patterns). The gap narrows on modern NVMe, but it never disappears.

B+Tree writes are fundamentally random: each insert must find its exact position in the tree and modify the page at that location. The page could be anywhere on disk.

LSM asks: what if we turned all writes into sequential appends?

### The Memtable -- all writes go here first

When Cassandra receives an insert for an `orders` row, it writes to the **Memtable**, an in-memory sorted data structure. Think of it as a sorted list living entirely in RAM:
```plaintext
Memtable (in memory):
┌──────────────────────────────────────────────┐
│ order: aaa-111, user: x, status: pending ... │
│ order: bbb-222, user: y, status: shipped ... │
│ order: ccc-333, user: x, status: delivered.. │
│ order: ddd-444, user: z, status: pending ... │
│                  ↑ sorted by order_id        │
└──────────────────────────────────────────────┘
```

Writing to the Memtable is fast because it's just RAM. It's sorted because reads need to find data efficiently. New inserts go to their sorted position in memory — no disk I/O involved in the write path at all, beyond the CommitLog (Cassandra's crash-recovery log, which is a sequential append and extremely fast).

### The SSTable - immutable and sorted

When the Memtable fills up, Cassandra flushes it to disk as an **SSTable** (Sorted String Table). This flush is a single sequential write, the entire sorted Memtable gets written from beginning to end in one pass. No random writes. No page modifications. Just a stream of sorted data to a new file.

Once written, **an SSTable is never modified**. It is immutable. Future writes don't touch it. This immutability is what makes the write path so clean: you never need to find a specific location on disk and modify it. You only ever write new files.

The immutability has an important consequence for updates and deletes. In a B+Tree, an update modifies the existing row in place. In an LSM Tree, an update writes a new version of the row to the current Memtable, which eventually becomes a new SSTable. Both the old version and the new version coexist on disk until compaction runs. Similarly, a delete doesn't remove data, it writes a **tombstone record** that marks the row as deleted. The old data persists until compaction.

### The read problem — read amplification

Over time, you accumulate multiple SSTables:
```plaintext
Disk state after several Memtable flushes:

SSTable-1 (oldest): [aaa-111] [ccc-333] [eee-555] [ggg-777]
SSTable-2:          [bbb-222] [ddd-444] [fff-666]
SSTable-3:          [aaa-111] [hhh-888]  ← updated version of aaa-111
SSTable-4 (newest): [iii-999] [jjj-000]
```

Now you query for `order_id = aaa-111`. Where is the latest version?

It could be in any SSTable. SSTable-1 has an old version. SSTable-3 has a newer version. To find the latest, the read path must check all four SSTables, compare the timestamps on any matching rows, and return the most recent one. This is **read amplification** — one logical read requires multiple physical reads across multiple files.

Before compaction, with many SSTables, reads are slow. Later post's benchmark puts a precise number on exactly this: Cassandra's cold read p99 before compaction is dramatically higher than PostgreSQL's — a gap that closes almost entirely once compaction runs and SSTable count drops to one.

### Bloom filters - the read shortcut

Reading every SSTable for every query would be unacceptably slow. Cassandra uses **Bloom filters** to short-circuit most of these checks.

A Bloom filter is a small, probabilistic data structure that answers one question: is this key *definitely not* in this SSTable?

The key word is *definitely not*. A Bloom filter can give you a false positive (it says "maybe yes" when the key isn't there) but it never gives you a false negative (if it says "definitely not," you can trust it). So before reading an SSTable, Cassandra checks the Bloom filter for that SSTable — if it says the key isn't there, you skip the entire SSTable. No disk read required.

In practice, Bloom filters eliminate most SSTable checks for most reads. Instead of reading 10 SSTables to find a key, you might read 1 or 2, the ones whose Bloom filters said "maybe." The filters live in memory, so checking them costs microseconds.

Bloom filters don't eliminate read amplification entirely, but they dramatically reduce it. They're why LSM reads are "slow but not catastrophic" rather than "completely unusable."

### Compaction - the background process that makes reads fast again

**Compaction** is the merge step that gives LSM its name. At regular intervals, Cassandra picks a set of SSTables and merges them into a single new SSTable:
```plaintext
Before compaction:
SSTable-1: [aaa-111 v1] [ccc-333]
SSTable-3: [aaa-111 v2] [hhh-888]

After compaction:
SSTable-merged: [aaa-111 v2] [ccc-333] [hhh-888]
```

During compaction, for any key that appears in multiple SSTables, only the latest version (highest timestamp) survives. Tombstones are eventually resolved and the deleted data is removed. Old SSTables are deleted after the merge completes.

After compaction, there are fewer SSTables, so reads check fewer files. Bloom filters cover fewer files. Read latency drops.

This is the mechanism behind the benchmark number: Cassandra's read latency fell from 4.1ms to 1.4ms after compaction. Same data, same hardware, fewer SSTables to check. The engine cleaned up after itself, and reads got faster as a result.

There are different compaction strategies with different tradeoffs — some optimize for write throughput, others for read consistency, others for time-series data. Post 4 covers these in detail.

---

## The Tradeoff Table

Here's how the three structures compare across the dimensions that matter:

| Property | B+Tree (PostgreSQL) | B-Tree (MongoDB) | LSM Tree (Cassandra) |
|---|---|---|---|
| **Write speed** | Moderate - random page writes | Moderate - similar to B+Tree | High - sequential appends |
| **Write variance** | High - page splits cause p99 spikes | High - same mechanism | Low - appends are uniform |
| **Point read speed** | Fast - log(n) tree traversal | Fast - similar, sometimes shortcut | Variable - depends on SSTable count and compaction state |
| **Range scan speed** | Fast - linked leaf list | Moderate - no linked leaves | Moderate - needs Bloom filter + SSTable scan |
| **Updates** | In-place modification | In-place modification | New write (old version persists until compaction) |
| **Deletes** | In-place (mark deleted) | In-place (mark deleted) | Tombstone write (data persists until compaction) |
| **Disk space** | Compact, matches live data | Compact, matches live data | Can exceed live data size (historical versions + tombstones) |
| **Read after heavy writes** | Consistent | Consistent | Degrades until compaction runs |

Neither structure is universally better. The table describes tradeoffs, not rankings.

---

## Why This Matters for Your Application

The theory translates directly to system design choices.

**Your workload is read-heavy with complex queries** - e-commerce product search, financial reporting, analytics over order history. Users query by multiple fields, run date range scans, join across tables. B+Tree wins here. PostgreSQL's linked leaf nodes make range scans fast. The write cost is acceptable because writes are infrequent relative to reads. The predictable read latency matters more than maximum write throughput.

**Your workload is write-heavy with known, fixed access patterns** - event ingestion, IoT sensor data, activity logs, append-heavy order pipelines. You're inserting millions of rows and reading them back by a known key pattern. LSM wins here. Cassandra's sequential write path handles sustained high-throughput inserts without the page-split variance that B+Tree would introduce. You pay in read complexity, but if your reads are simple key lookups or partition scans, Bloom filters and compaction keep that cost manageable.

Most real workloads are somewhere in the middle, which is why MongoDB exists in the space between the two extremes - a B-Tree engine with flexible documents, good write performance, and reasonable query flexibility.

Post 5 will put real numbers on these tradeoffs. When you see Cassandra's write throughput compared to PostgreSQL's on the same hardware, the gap will be exactly what the data structure analysis predicts.

---

## What's Next: The Theory Gets Concrete

You now have the foundation: B+Tree for reads, LSM for writes, and a B-Tree sitting comfortably in between. You know why page splits cause latency spikes. You know why Cassandra reads degrade with SSTable accumulation and recover after compaction.

Next Post takes everything you just learned and shows it working inside real database engines.

We're going inside PostgreSQL and MongoDB - from the moment a `SELECT` arrives, through the buffer pool, down the B+Tree, out to the heap file, and back. We'll trace a write through the WAL and watch it land in shared_buffers. We'll see what MVCC actually looks like inside a heap page, and why a PostgreSQL UPDATE doesn't modify a row - it writes a new one.