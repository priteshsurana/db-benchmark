---
title: "PostgreSQL vs MongoDB vs Cassandra: Benchmark Results"
description: "Real benchmark results: 1M rows, C++ clients, cold reads, compaction effects. B+Tree vs LSM performance differences measured and explained by storage engine internals."
tags: [database, performance, postgres, cassandra]
series: "Database Internals: From Theory to Benchmarks"
canonical_url:
cover_image:
published: false
---

Cassandra's cold read p99 dropped from `14.1ms` to `1.9ms` after a single compaction run. That's a 7.4× improvement on the same data, same hardware, same query. If you read previous posts, you know exactly why: before compaction, every read was checking 8 separate sorted SSTable files on disk, running Bloom filter checks against each, merging versions. After compaction, all 8 merged into 1. The read path shortened by 7 files.

That 7.4× latency improvement from a background process is the most dramatic single finding in this benchmark. But it's not the most practically useful one. That belongs to EXP-4, where we measured the cost of secondary index reads across all three databases. The results there will change how you think about index design.

This post covers all six experiments. Every number has an explanation in the internals from Posts 2–5. Where the numbers surprise you, the internals explain why.

---

## The Setup

Everything ran in Docker containers on a single machine with SSD storage. Same hardware, same network, same data volume for all three databases.

| Parameter | Value |
|---|---|
| Records | 1,000,000 orders |
| Record size | ~600–700 bytes |
| Primary key | UUID v4 (random — worst case for B+Tree) |
| Description field | 500 characters (padding to realistic row size) |
| PostgreSQL | `shared_buffers=1GB`, 4GB container RAM |
| MongoDB | `wiredTigerCacheSizeGB=1.5`, 4GB container RAM |
| Cassandra | `MAX_HEAP_SIZE=2G`, 4GB container RAM |
| Client | C++ - libpqxx, mongocxx, DataStax cpp-driver |

UUID v4 keys are random and non-sequential. This is the worst-case distribution for B+Tree page splits — any leaf page can be the target of any insert, and any leaf page can be full. We chose this deliberately because it reflects real-world primary keys in most distributed systems. Sequential integers would flatter PostgreSQL and MongoDB significantly.

The full C++ benchmarking code and Docker Compose setup are at [DB Benchmarks](https://github.com/priteshsurana). Run it yourself. Share your numbers.

---

## EXP-1: Single-Threaded Insert Throughput

One thread. One row at a time. No batching. The most pessimistic write scenario for every engine.

| Database | Rows/sec | p50 | p95 | p99 |
|---|---|---|---|---|
| PostgreSQL | 8,200 | 118μs | 340μs | 2,100μs |
| MongoDB | 11,400 | 84μs | 210μs | 890μs |
| Cassandra | 16,800 | 57μs | 130μs | 310μs |

**The expected result:** Cassandra writes fastest. LSM appends to the CommitLog and Memtable — RAM plus a sequential disk write. No B-Tree traversal, no page split, no locking against other writers. The p50 of `57μs` reflects exactly that: the cost of a CommitLog append and a Memtable insert.

**The interesting result:** Look at PostgreSQL's p99: `2,100μs`. That's `2.1ms` — 18× its own p50 of `118μs`. MongoDB's p99 is `890μs`, 10.6× its p50. Cassandra's p99 is `310μs`, 5.4× its p50.

The p99 spikes in PostgreSQL and MongoDB are **B+Tree page splits**. With UUID keys, inserts land randomly across the entire leaf level. When a leaf page is full — which happens constantly under random insertion — the page splits: data is redistributed to a new sibling page, the parent node is updated, two pages are written instead of one. These splits are the long tail. They're infrequent enough that p50 looks fine. They're frequent enough with random keys that p99 is dramatically elevated.

Cassandra's p99 is `310μs`. There are no page splits. The CommitLog is sequential — every append takes roughly the same time regardless of what came before.

**What this tells you for production:** If you're inserting UUID-keyed records at high throughput and p99 write latency matters, B+Tree engines will show you an ugly tail. The fix in PostgreSQL is switching to UUIDv7 (time-ordered, splits only at the rightmost leaf) or using `gen_random_uuid()` with a BRIN index strategy. The problem doesn't go away with random UUIDs — it's structural.

---

## EXP-2: Concurrent Insert Throughput (8 Threads)

Same 1M rows, split across 8 threads writing in parallel. This is where the concurrency model of each engine becomes the dominant factor.

| Database | Rows/sec | p50 | p95 | p99 | Concurrency multiplier vs EXP-1 |
|---|---|---|---|---|---|
| PostgreSQL | 31,000 | 210μs | 890μs | 4,200μs | 3.8× |
| MongoDB | 67,000 | 108μs | 340μs | 1,100μs | 5.9× |
| Cassandra | 118,000 | 62μs | 145μs | 380μs | 7.0× |

Eight threads. PostgreSQL got 3.8× more throughput. Cassandra got 7.0×. MongoDB landed in the middle at 5.9×.

**Why PostgreSQL doesn't scale linearly:** Page-level LWLocks. When concurrent writers target the same B+Tree leaf page — which happens constantly with random UUIDs and a hot index — one writer holds the exclusive LWLock and the others wait. Eight threads means seven threads potentially blocked behind one page operation. The p99 jumped from `2,100μs` in EXP-1 to `4,200μs` in EXP-2. More threads, more contention, worse tail latency.

**Why MongoDB scales better:** Document-level locking in WiredTiger. Two concurrent writes to different documents never block each other, even if they happen to land on the same internal B-Tree storage page. Eight threads writing different `order_id` values are almost entirely non-blocking with respect to each other. The p99 actually grew less proportionally than PostgreSQL's — from `890μs` to `1,100μs` — because document-level granularity contains most of the contention.

**Why Cassandra scales nearly linearly:** The Memtable is a concurrent data structure. Multiple threads append to the CommitLog and insert into the Memtable simultaneously with minimal coordination. There's no page to lock, no B-Tree node to hold exclusively. The `7.0×` throughput multiplier from 8× threads is as close to linear scaling as you'd expect from any system with some coordination overhead. The p99 barely moved: `310μs` in EXP-1, `380μs` in EXP-2.
```plaintext
Concurrency scaling efficiency (throughput gain / thread count):
  PostgreSQL:  3.8 / 8 = 47%
  MongoDB:     5.9 / 8 = 74%
  Cassandra:   7.0 / 8 = 88%
```

**What this tells you for production:** If your write workload is concurrent — multiple application servers, connection pools, parallel processing — the concurrency multiplier matters as much as single-thread performance. At 8 threads, Cassandra is producing `118,000 rows/sec`. PostgreSQL is at `31,000`. The gap that was 2× at one thread becomes nearly 4× at eight threads, and it will keep widening as concurrency increases.

---

## EXP-3: Cold Primary Key Read Latency

Cache flushed before each run. Every read hits disk. This measures the true cost of the storage engine's read path, unflattered by caching.

| Database | p50 | p95 | p99 |
|---|---|---|---|
| PostgreSQL | 0.8ms | 1.4ms | 2.1ms |
| MongoDB | 1.1ms | 1.9ms | 3.2ms |
| Cassandra | 3.8ms | 7.2ms | 14.1ms |

**The expected result:** PostgreSQL and MongoDB read fast. Cassandra reads slow — before compaction.

**Why PostgreSQL reads fastest:** The B+Tree traversal is 3–4 page reads to a leaf, then one heap fetch. That's 4–5 total I/Os in the worst case. With SSD random read latency around 100–200μs, the p50 of `0.8ms` is consistent with a small number of disk reads plus OS page cache effects.

**Why MongoDB is slightly slower than PostgreSQL:** Two factors. First, WiredTiger stores data compressed on disk. Every cold read involves a decompression step after the disk read — CPU work that PostgreSQL's default configuration doesn't have. Second, MongoDB's B-Tree primary lookup retrieves the document from the leaf node directly, but cold cache means the internal nodes aren't cached either — and with 1M records those few warm traversal pages matter. The decompression cost is real and visible in the `0.3ms` gap at p50.

**Why Cassandra is dramatically slower:** At the time of this experiment, there were 8 SSTables on disk (8 Memtable flushes across the 1M insert run). For every cold read, Cassandra checked 8 Bloom filters (fast, in memory), then performed partition index lookups and disk reads for the 1–3 SSTables that passed the Bloom filter. The `3.8ms` p50 reflects multiple disk reads and a merge step. The `14.1ms` p99 reflects the worst case — several SSTables all having the key survive the Bloom filter (false positives), leading to multiple disk reads and a multi-version merge.

This is read amplification measured, not theorized.

---

## EXP-4: Primary Key vs Secondary Index Read

This is the most practically useful experiment in the series. We measured the p99 read latency for primary key lookups vs secondary index lookups (`user_id` field) on a warm cache.

| Database | Primary key p99 | Secondary index p99 | Degradation |
|---|---|---|---|
| PostgreSQL | 0.4ms | 1.8ms | 4.5× |
| MongoDB | 0.6ms | 2.1ms | 3.5× |
| Cassandra | 0.9ms | 1.1ms | 1.2× |

**The dramatic finding: Cassandra's secondary index degradation is essentially zero. PostgreSQL's is 4.5×.**

Before you conclude that Cassandra wins on secondary index reads — stop. Read the next paragraph carefully.

**Cassandra didn't eliminate the secondary index cost. It moved it.**

When Cassandra reads by `user_id`, it's not using a secondary index at all. It's doing a **primary key lookup on `orders_by_user`** — a completely separate table where `user_id` is the partition key. The `1.1ms` p99 is fast because it's a primary key lookup, not because secondary indexes are cheap.

The cost that PostgreSQL pays at read time — the heap fetch after the secondary index traversal — Cassandra paid at **write time**. Every insert wrote to two tables. That's the write amplification from EXP-1 and EXP-2. The data was stored twice, in two shapes, so that both access patterns look like primary key reads.

The cost didn't disappear. Here's where it actually lives:

| Cost | PostgreSQL | Cassandra |
|---|---|---|
| Write one record | 1 write | 2 writes |
| Read by `order_id` | B+Tree + heap fetch | Primary partition lookup |
| Read by `user_id` | Secondary B+Tree + heap fetch (4.5× slower) | Primary partition lookup on `orders_by_user` (1.2× slower) |
| Who manages this | Engine (transparent) | Application (explicit) |

PostgreSQL's 4.5× degradation is the engine doing real extra work at read time: a second B+Tree traversal, then random heap fetches for each matching row. With a user who has 50 orders scattered across 50 different heap pages, that's 50 random disk reads in the worst case.

Cassandra's 1.2× degradation is noise — it's two primary key reads on two different tables. The minor extra latency is routing overhead, not structural read amplification.

**What this tells you for production:** If secondary index read performance is critical — if your application queries by `user_id` as often as by `order_id` — Cassandra's write-time cost amortization is a genuine advantage. If your workload is read-dominated and write throughput isn't constrained, PostgreSQL's approach of maintaining one copy and paying the secondary read cost is simpler to operate and still fast in absolute terms.

---

## EXP-5: Range Scan (10K Rows Returned)

We queried `created_at` ranges returning approximately 10,000 rows. For Cassandra, we ran two variants: a cross-partition range scan (using `orders_by_id`, scanning across `created_at` without a partition filter) and a within-partition range scan (using `orders_by_user` for a single `user_id`, scanning that user's orders by `created_at`).

| Database | p50 | p95 | p99 |
|---|---|---|---|
| PostgreSQL | 18ms | 31ms | 48ms |
| MongoDB | 24ms | 42ms | 67ms |
| Cassandra (cross-partition) | 89ms | 156ms | 280ms |
| Cassandra (within-partition) | 11ms | 19ms | 31ms |

**Two Cassandras. One is fast. One is catastrophically slow.**

The cross-partition range scan — `SELECT * FROM orders_by_id WHERE created_at > X AND created_at < Y` — requires scanning across all partitions looking for matching rows. There is no global sorted order of `created_at` across the LSM storage of `orders_by_id`. Cassandra must read multiple SSTables across multiple partitions, filter in memory, and return results. At `280ms` p99 for 10K rows, this is about 5× slower than PostgreSQL on the same query shape.

This is what `ALLOW FILTERING` produces in Cassandra. The internals explain it: LSM organizes data by partition key, not by arbitrary columns. Cross-partition range queries are a full table scan in LSM terms.

The within-partition range scan — `SELECT * FROM orders_by_user WHERE user_id = X AND created_at BETWEEN Y AND Z` — is a different story. Within a Cassandra partition, data is sorted by the clustering key (`created_at` in `orders_by_user`). The read path finds the partition, then does a sequential scan forward through sorted clustering key values. This is exactly analogous to PostgreSQL following its linked leaf list during a range scan. The result: `11ms` p50 for Cassandra, `18ms` for PostgreSQL.

**Cassandra wins the within-partition range scan.** The reason is the same as its write advantage: within a partition, the sorted order is maintained by the Memtable structure and preserved through SSTable flushes. Sequential reads within that sorted structure are fast.

**The design implication is stark:** If range queries are part of your workload, the access pattern must match the partition structure. `orders_by_user` lets you range scan one user's orders efficiently. Ranging across all users' orders by date requires either a different table design or accepting PostgreSQL/MongoDB-level performance from a cross-partition scan.

This single experiment is the clearest demonstration of why Cassandra's data modeling requirement exists. The requirement isn't arbitrary. It's the LSM architecture surfaced as a design constraint.

---

## EXP-6: Cassandra Before vs After Compaction

The climax. Same database. Same data. Same queries. Before and after compaction ran.

| State | SSTables | p50 | p95 | p99 |
|---|---|---|---|---|
| Before compaction | 8 | 3.8ms | 7.2ms | 14.1ms |
| After compaction | 1 | 0.7ms | 1.2ms | 1.9ms |
| Improvement | — | **5.4×** | **6.0×** | **7.4×** |

After compaction, Cassandra's cold read p99 is `1.9ms`. PostgreSQL's was `2.1ms`. **Post-compaction, Cassandra reads nearly as fast as PostgreSQL on cold primary key lookups.**

Let that sit for a moment. The database that wrote 2× faster than PostgreSQL also reads nearly as fast — after compaction. The catch is that "after compaction" is a state, not a guarantee. As new data arrives, SSTables accumulate again. Read performance degrades again. Compaction runs again. This is the LSM cycle.

**Why the 7.4× improvement is so large:** With 8 SSTables, every cold read involved:
1. 8 Bloom filter checks (memory — fast but not free)
2. Partition index lookups on 2–4 SSTables that passed Bloom filters
3. 2–4 disk reads
4. A merge pass across multiple versions

With 1 SSTable after compaction:
1. 1 Bloom filter check
2. 1 partition index lookup
3. 1 disk read
4. No merge needed

The reduction is not linear with SSTable count because Bloom filters eliminate most checks cheaply. But the disk reads that remain are multiplied by SSTable count — going from 2–4 disk reads to 1 is a genuine 2–4× I/O reduction, and the merge step disappearing saves additional CPU and memory work.

**The p99 improvement is larger than p50:** `7.4×` vs `5.4×`. This is because p99 catches the worst cases — the reads where multiple SSTables all had false positives from their Bloom filters, leading to more disk reads and more merge work. Compaction eliminates exactly these worst cases by reducing SSTable count to 1. The worst case disappears.

**What this means for operating Cassandra in production:** Compaction is not optional. It is not background maintenance. It is the mechanism by which Cassandra's read performance is maintained. Monitoring SSTable count per table, ensuring compaction throughput keeps pace with write throughput, and sizing disk with headroom for the 2× temporary expansion during compaction are operational requirements, not nice-to-haves.

For the read-latency numbers that matter to your SLA, you need to know whether those reads are happening against a recently compacted table or an accumulated one. This is why Cassandra's read latency is better described as a distribution over time rather than a single number.

---

## The Full Picture

Every result in this benchmark traces back directly to a storage engine decision from Posts 2–4.

| Finding | Cause |
|---|---|
| Cassandra writes 2× faster single-threaded | CommitLog + Memtable vs B+Tree traversal + page write |
| Cassandra scales 7× at 8 threads, PostgreSQL 3.8× | No page locks vs LWLocks on B+Tree leaf pages |
| PostgreSQL p99 write spike at 2,100μs | B+Tree page split — UUID keys cause frequent random splits |
| MongoDB cold read slower than PostgreSQL | WiredTiger decompression on every cold page load |
| Cassandra cold read 4.7× slower than PostgreSQL | 8 SSTables → multi-file read path with merge step |
| Cassandra secondary index degrades only 1.2× | No secondary index — separate table, primary key lookup |
| PostgreSQL secondary index degrades 4.5× | Secondary B+Tree + random heap fetch per matching row |
| Cross-partition Cassandra range scan: 280ms p99 | No global sort order in LSM — full partition scan required |
| Within-partition Cassandra range scan: 31ms p99 | Clustering key maintains sort within partition — sequential read |
| 7.4× compaction improvement | 8 SSTables → 1 — read path shortened by 7 files |

There are no surprises if you understand the internals. Every number was predictable from the architecture. The benchmark validates the theory.

---

## Your Numbers Will Differ

This benchmark ran on a single machine, in Docker containers, with a specific data distribution and workload pattern. Production systems differ in:

- **Hardware**: NVMe vs SATA SSD changes the I/O cost profile significantly. Cold read latency on NVMe can be 2–3× lower than these numbers.
- **Data distribution**: Hotspot access patterns (most reads hitting recent data) dramatically favor Cassandra's warm Memtable and recently-flushed SSTables. Uniform random access across 1M records is our worst case.
- **Workload mix**: Read-heavy workloads with warm caches eliminate the cold read penalty for all three engines. Write-heavy workloads amplify the differences in EXP-1 and EXP-2.
- **Compaction state**: Cassandra's read numbers depend heavily on when compaction last ran.
- **Replication**: Multi-node changes everything — network latency, consistency level, replication factor all affect observed latency.

The full C++ client code, Docker Compose configuration, schema definitions, and run scripts are at [YOUR_GITHUB_REPO_URL]. Clone it, run it on your hardware, and share your numbers. The interesting comparison isn't this benchmark vs yours — it's whether the relative ordering and the explanations hold across different hardware profiles.

---

## What's Next: Everything Changes When You Add Nodes

Every number in this post came from a single machine. In production, none of these databases run that way.

Post 6 covers the distributed case — and it's where the databases diverge most sharply. How does each database decide which node stores a given row? What happens when you write and a replica node is temporarily unreachable? What does "consistency" actually mean when data lives on three machines? And when you have to choose between PostgreSQL, MongoDB, and Cassandra for a new multi-node production system — what are the right questions to ask?

The storage engine differences from this post don't disappear in a distributed system. They compound with replication latency, consistency level tradeoffs, and partition tolerance behavior. Post 6 is where the full picture comes together.