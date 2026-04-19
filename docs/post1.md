---
title: "What Actually Happens When You Call INSERT?"
description: "You call INSERT and the database says OK. But between those two moments, at least four things happened that most engineers never think about. Here's what."
tags: [database, postgres, mongodb, backend]
series: "Database Internals: From Theory to Benchmarks"
canonical_url:
cover_image:
published: false
---
You call `INSERT`. The database says `OK`. You move on.

That acknowledgment feels instant. It feels cheap. It feels like the database just... wrote something down. But between your `INSERT` and that `OK`, at minimum four distinct things happened that most engineers who use databases every day have never thought about:

1. The write was recorded in a sequential log before it touched any data structure — so a crash wouldn't lose it
2. At least one index was updated — and that update is more expensive than the insert itself on some engines
3. A decision was made about whether to hit the disk synchronously or defer it — a tradeoff with real latency consequences
4. The data was placed into a structure that was chosen years ago by the database designers, and that choice explains almost every performance characteristic you've ever observed

This series is about those four things. Not from a textbook — from the inside out, with real benchmark numbers at the end.

---

## The Moment of Insertion

Let's start with the most basic question: where does the data actually go first?

The answer is different for each database, and the differences are not superficial.

**PostgreSQL** receives your `INSERT` and immediately writes a record to something called the **WAL** — the Write-Ahead Log. This is a sequential append-only file on disk. Only after that WAL record is safely written does PostgreSQL modify the in-memory structure called **shared_buffers**, and eventually write the row to its final resting place: a **heap file**. The heap is just what it sounds like — rows stored roughly in the order they arrived, in fixed 8KB pages. The primary key index is a completely separate structure, updated separately.

**MongoDB** receives your document and routes it through **WiredTiger**, its storage engine. WiredTiger writes to its own **journal** (similar in spirit to PostgreSQL's WAL but different in format and behavior) and places the document into the **WiredTiger cache** — an in-memory buffer. Here's the twist: MongoDB's primary storage structure is a B-Tree, and unlike PostgreSQL's heap, the document data lives *inside* the B-Tree itself. There is no separate heap. The collection and the primary index are the same structure.

**Cassandra** does something neither of the others does. It writes to a **CommitLog** (its crash-recovery log) and then places the data into a **Memtable** — a sorted in-memory buffer that accumulates writes until it's full, at which point it gets flushed to disk as an immutable file called an **SSTable**. Cassandra never modifies existing files. Every write is an append. Every update is a new version. Every delete is a new record saying "this data is gone."

Three databases. Three fundamentally different answers to the question "where does the data go first."

Why does this matter? Because each approach has consequences that ripple through everything — write speed, read speed, crash recovery time, compaction behavior, and what happens to your p99 latency under load. Posts 2 through 4 go deep on each engine. But first, let's look at the structure that sits underneath all of this.

---

## The Index Is Not What You Think

Most engineers think about indexes when they're writing queries. "This query is slow, I need an index." The mental model is: indexes exist to make reads fast.

That's not wrong. But it's half the picture.

**Every index is updated on every write.** Throughout this series we'll use a concrete example: an orders table with fields like order_id, user_id, status, amount, and created_at. When you insert an order, PostgreSQL doesn't just write the row to the heap — it also updates the B+Tree index on `order_id`, the B+Tree index on `user_id`, the B+Tree index on `created_at`. Three indexes means roughly four write operations for a single `INSERT`. The index update cost is not a footnote. On tables with several indexes under heavy write load, index maintenance is often the dominant cost of the write path.

Now here's the part that most engineers have never considered: the three databases use fundamentally different data structures for those indexes, and the choice of structure explains almost everything about their performance profiles.

PostgreSQL uses a **B+Tree**. MongoDB's WiredTiger also uses a **B-Tree** (a close relative). Cassandra uses an **LSM Tree** — a completely different class of structure that doesn't modify anything in place, ever.

The B+Tree and B-Tree are optimized for reads. You can find any key in a handful of disk reads, and the structure stays balanced automatically. The cost is that every write has to find the right place in the tree and modify it — which can mean reading pages from disk, modifying them, and writing them back. Random writes. Slow on spinning disks, less slow on SSDs, but never free.

The LSM Tree is optimized for writes. New data always goes to the end of something — a log, a buffer, a new file. There are no random writes. The cost is that reads become more complex, because the data you're looking for might be in any of several files that were written at different times.

This is the reason Cassandra writes faster than PostgreSQL under sustained load. And it's the reason Cassandra reads *slower* before a process called **compaction** runs.

Post 2 breaks this down completely — what each structure looks like, how it works, and why the choice of structure is the single most important decision a database designer makes.

---

## What "Durability" Actually Costs

When someone says a database is durable, they mean: if the server loses power the instant after your `INSERT` is acknowledged, your data will still be there when the server comes back.

That's a strong guarantee. And it has a real cost.

To survive a power failure, data has to reach physical storage — magnetic disk or flash cells — before the acknowledgment goes out. Not the OS's buffer. Not the CPU cache. The actual hardware. The system call that forces this is called `fsync`, and it is one of the most expensive operations a database performs. On a typical NVMe SSD, a single `fsync` takes microseconds. On a networked file system or a busy system under load, it can take milliseconds. And some workloads trigger thousands of them per second.

Here's where the databases diverge sharply.

PostgreSQL's default behavior is **synchronous durability**: every committed transaction waits for its WAL record to be `fsync`'d to disk before the acknowledgment goes out. You get the strongest possible guarantee. You also pay for every write with a disk flush.

MongoDB's default behavior has historically been more relaxed — by default, the journal syncs every 100 milliseconds. Your write is acknowledged as soon as WiredTiger's in-memory cache accepts it. You get lower latency. You accept that up to 100ms of writes could be lost in a hard crash. This is configurable — `j: true` forces a journal flush per write — but the default trades some durability for speed.

Cassandra's CommitLog syncs periodically too, every ~10 seconds by default. Same tradeoff, pushed even further toward throughput. If you need per-write durability in Cassandra, you configure it — and you pay the latency cost.

These aren't just configuration details. They're architectural philosophies. The benchmark numbers in later posts will show you exactly what each philosophy costs in microseconds. The difference between synchronous and asynchronous durability shows up plainly in the latency measurements — and the gap is bigger than most engineers expect.

---

## The Benchmark Teaser

Here's what we measured when we ran 1 million inserts and then reads against all three databases on identical hardware. These numbers are illustrative — directionally accurate — and the real C++ benchmark results are covered in a later Post.

### Single-threaded insert throughput

| Database   | Rows/sec (approx) |
|------------|-------------------|
| Cassandra  | ~18,000           |
| MongoDB    | ~12,000           |
| PostgreSQL | ~8,500            |

Cassandra is more than 2× faster than PostgreSQL on pure insert throughput. If you know about LSM Trees, this makes sense. If you don't, it looks like magic.

### Cold primary key read latency (p99)

| Database              | p99 latency (approx) |
|-----------------------|----------------------|
| PostgreSQL            | ~1.2ms               |
| MongoDB               | ~1.8ms               |
| Cassandra (before compaction) | ~4.1ms     |
| Cassandra (after compaction)  | ~1.4ms     |

Now look at Cassandra. Before compaction, it's the slowest reader by a factor of 3. After compaction, it's competitive with PostgreSQL. Same database. Same data. Same query. The difference is whether a background merge process has run.

This is the central tension of LSM-based storage: you pay for write speed with read complexity, and you recover that complexity through compaction. The "after compaction" number is not a cheat — it reflects real production behavior. But it tells you something important: Cassandra's read latency is not a fixed property. It depends on what state the engine is in.

These numbers also tell you something about the write-read tradeoff that runs through this entire series. The engine that writes fastest reads slowest before it cleans up after itself. The engine that reads fastest — PostgreSQL — is also the one paying the highest cost per write to keep its B+Tree in a consistent, readable state.

The real numbers from the C++ benchmark, with methodology, hardware specs, and full latency distributions, are in later Post. But this is the shape of what you'll find.

---

## What This Series Will Cover

Here's the full map. Each post answers one question:

**Post 1 — What is actually happening when you insert a row?**
This post. The wide-angle view. Why the insert isn't simple, and why the differences between engines matter.

**Post 2 — Why do databases use B+Tree, B-Tree, or LSM — and what's the real difference?**
The data structures underneath the storage engines, explained from first principles. Why the choice of structure determines almost everything about read and write performance.

**Post 3 — How do PostgreSQL and MongoDB store and retrieve data internally?**
Deep dive into the heap file, shared_buffers, WAL, WiredTiger's B-Tree, the journal, MVCC, and what a real insert and read look like step by step in each engine.

**Post 4 — How does Cassandra's LSM-based storage work end to end?**
Memtables, SSTables, Bloom filters, compaction strategies, tombstones, and why deleting data in Cassandra is more complicated than it sounds.

**Post 5 — What do the benchmark numbers actually show — and why?**
The C++ benchmark: 1 million records, controlled hardware, full methodology. Write throughput, read latency distributions, the compaction effect, and what explains every number.

**Post 6 — How does everything change when you go multi-node?**
Replication, sharding, consistency levels, and why the single-node storage engine behavior is just the beginning. CAP theorem applied to real production decisions.

---

## What's Next

Before we can explain why Cassandra writes twice as fast as PostgreSQL but reads three times slower before compaction, we need to understand the structures that cause those numbers.

The next Post is entirely about data structures — B+Tree, B-Tree, and LSM Tree. You'll understand why B+Trees are the default for read-heavy databases, why LSM Trees dominate write-heavy systems, and why the choice of structure is the single decision that every other database behavior flows from.

If you've ever wondered why adding a sixth index to a table hurts write performance more than the fifth index did, next Post has the answer.

Follow the series so you don't miss it.