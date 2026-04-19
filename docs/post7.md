---
title: "PostgreSQL vs MongoDB vs Cassandra: Multi-Node and How to Choose"
description: "Replication, sharding, CAP tradeoffs, and a decision framework for choosing between PostgreSQL, MongoDB, and Cassandra in a distributed production system."
tags: [database, backend, postgres, cassandra]
series: "Database Internals: From Theory to Benchmarks"
canonical_url:
cover_image:
published: false
---
I know I mentioned post 6 will be about the benchmark numbers but to limit the series at 6 parts, I have the benchmark numbers here 
Everything in Posts 2 through 5 - the B+Tree structure, the LSM write path, the compaction effect, the secondary index cost, happens on a single machine. The performance differences are real and they matter. But the moment you add a second node, the binding constraint shifts entirely. Network latency between nodes is measured in milliseconds. The p50 write latency difference between PostgreSQL and Cassandra on a single node is measured in microseconds. The network is 100× slower than the storage engine. It doesn't matter how fast your CommitLog append is if the replication acknowledgment takes 5ms across a data center.

Multi-node also introduces a class of failure that doesn't exist on a single machine: the network partition. Two nodes that can't reach each other, each believing they're the healthy one. When that happens, every database must make a choice. And the choice each one makes reveals its fundamental design philosophy more clearly than any benchmark.

This post covers how each database behaves at the cluster level, then ends with the decision framework the whole series has been building toward.

---

## PostgreSQL Multi-Node

### The honest starting point

PostgreSQL was designed as a single-node database. It's the most important thing to understand before running PostgreSQL at scale. Every multi-node capability PostgreSQL has - streaming replication, logical replication, connection pooling, sharding, was added on top of a core that was built to run on one machine, with one set of files, with one process managing all reads and writes. The foundation is excellent. The multi-node layer requires additional tooling and carries additional operational complexity as a result.

If you are evaluating PostgreSQL for a distributed system, you are evaluating PostgreSQL plus the tooling around it: Patroni or repmgr for high availability, PgBouncer for connection pooling, and potentially Citus for horizontal write scaling. Each is mature and well-understood. None is built in.

### Streaming replication

When a write lands on the PostgreSQL primary, it follows the path from Post 3: WAL record written, shared_buffers updated, heap and index pages modified. In a replicated cluster, the WAL record does a second job: it's also shipped to standby nodes via **streaming replication**.

A background process called the WAL sender on the primary streams WAL records to one or more standby nodes in near real time. Each standby has a WAL receiver that writes those records to its own WAL, and a recovery process that replays them against the standby's heap and index files. The standby is perpetually in crash recovery mode i.e. applying WAL from the primary rather than from a local crash.

This is the same WAL mechanism from Post 3, but the purpose is different. In crash recovery, WAL is replaying changes to reconstruct a consistent state after a failure. In streaming replication, WAL is shipping live changes to a secondary machine to keep it current.

**Synchronous vs asynchronous replication** is the most consequential configuration choice in a PostgreSQL cluster:

In **asynchronous** mode (the default), the primary writes its WAL and acknowledges the commit to the application immediately. The WAL sender ships the record to standbys afterward, but the primary doesn't wait. This means commits are fast. The application sees write latency close to local WAL flush time. The risk: if the primary crashes before the WAL record reaches a standby, and that standby is promoted to primary, those last few committed transactions are gone. The application was told "committed" and the data doesn't exist on the new primary.

In **synchronous** mode, the primary waits for at least one designated standby to confirm receipt of the WAL record before acknowledging the commit. This eliminates the data loss window but adds the round-trip latency to the standby so every write now pays network cost. In the same data center, this is typically 1–3ms. Across data centers, it can be 50–100ms. Whether that cost is acceptable depends entirely on your write SLA.

**Replication lag** is the delay between when a WAL record is generated on the primary and when it's applied on a standby. Under normal conditions, lag is milliseconds. Under heavy write load or network contention, it can grow to seconds. If your application writes on the primary and immediately reads from a standby, the data may not be there yet. This "read your own writes" consistency failure is the most common source of subtle bugs in PostgreSQL deployments that use read replicas.

### Read scaling vs write scaling

Read scaling with PostgreSQL is straightforward. Add more standbys, route read traffic to them. Each standby is a full copy of the data, capable of serving any read query. This works well and is the standard pattern for PostgreSQL at read-heavy scale.

Write scaling is fundamentally different. All writes must go to the single primary. There is no mechanism in base PostgreSQL to accept writes on two nodes simultaneously. The primary is the sole source of truth, and its write throughput is bounded by what one machine can handle — roughly the limits you saw in Post 6's single-node numbers.

**Citus** is the primary extension for horizontal write scaling. It adds sharding on top of PostgreSQL by distributing rows across worker nodes based on a distribution column. For the `orders` table, you'd distribute by `order_id` so each worker owns a range of `order_id` values and handles reads and writes for that range. A coordinator node routes queries to the appropriate worker. Single-shard queries i.e. lookups by `order_id`,  route to one worker efficiently. Multi-shard queries aggregations across all orders fan out to all workers and aggregate at the coordinator.

Citus works and is used in production at significant scale. But consider what you're taking on: a coordinator that can become a bottleneck for multi-shard queries, shard rebalancing operations when you add workers, and the coordination overhead below.

### Transactions across nodes

On a single primary, PostgreSQL transactions are fully ACID. In a Citus sharded cluster, a transaction that touches rows on multiple shards requires **two-phase commit (2PC)**.

2PC runs in two rounds: first, the coordinator asks each involved shard to prepare (lock the rows, write the WAL, but don't commit yet). If all shards confirm they're prepared, the coordinator sends commit to all of them. If any shard fails during preparation, the coordinator sends rollback.

The cost is real: two network round trips to all participating shards, plus locks held during the prepare phase. Under high concurrency, prepared transactions holding locks become a contention source. The failure mode like a coordinator crash between prepare and commit leaves shards in a prepared-but-undecided state that requires manual recovery. 2PC is correct, mature, and operationally manageable. It is also slower and more complex than a local transaction.

This is one of PostgreSQL's genuine scaling limitations: the richer the transaction semantics, the harder it is to distribute.

### CAP position

PostgreSQL is **CP** meaning it prioritizes consistency over availability during a network partition. When the network separating the primary from its standbys is partitioned, the primary doesn't know if standbys are down or if *it* is the isolated node. With automatic failover tooling (Patroni, repmgr), the cluster will eventually elect a new primary from the standby side. But during the election window, writes may be unavailable. PostgreSQL will not allow two nodes to simultaneously believe they're the primary and accept writes, because that would produce divergent state with no way to reconcile it.

The practical consequence: during a partition event, your application may see write unavailability for the duration of the failover typically seconds to tens of seconds depending on your tooling's detection and election timeout configuration. Reads on standbys may continue; writes are blocked until leadership is resolved. For systems where write availability is more important than strict consistency, this is a constraint to plan around.

---

## MongoDB Multi-Node

### Replica sets - replication as a prime feature

MongoDB designed replication as a native feature from early in its history, and it shows. Spinning up a 3-node replica set is a supported, documented, first-class operation. There's no external Patroni equivalent required for basic scalling.

A replica set consists of one primary and one or more secondary nodes. The primary accepts all writes. Secondaries replicate from the primary and can serve reads. Automatic failover happens via an election: when secondaries can't reach the primary, they hold an election among themselves and promote the node with the most up-to-date oplog. The entire process - detection, election, promotion typically completes in under 10 seconds with default settings.

The replication mechanism is the **oplog** (operations log), a special capped collection in the `local` database on every replica set member. When WiredTiger applies a write to the primary's collection B-Tree, MongoDB also appends a logical description of that operation to the oplog: "insert this document with this `_id`" or "update this field on this document."

Secondaries continuously tail the primary's oplog and apply each operation to their own WiredTiger storage. This is structurally similar to PostgreSQL's WAL streaming, but the oplog is **logical** meaning it records operations at a higher semantic level while PostgreSQL's WAL is **physical** so it records exact byte changes to specific pages. The logical oplog is more portable and can be consumed by change data capture systems. The physical WAL is more efficient for pure replication because it doesn't require re-interpreting operations.

### Write concern in a cluster

You know write concern from Post 4's single-node discussion. In a replica set, the options become meaningfully different:

**`w: 1`** acknowledges as soon as the primary's WiredTiger cache and journal accept the write. Fast. But if the primary crashes before replicating and a secondary is promoted, that write is gone. The new primary never had it. MongoDB will write the un-replicated operations to a rollback file when the old primary rejoins, but the application already received "success."

**`w: majority`** waits until a majority of voting replica set members (for a 3-node set, that's 2 nodes) have written the operation to their journals. This write survives primary failure. If the primary crashes immediately after acknowledging, the operation is on at least one secondary. The newly elected primary will have it. The cost is the round-trip to the nearest secondary that confirms typically in a few milliseconds in the same data center.

The right choice depends entirely on your durability requirements. Financial writes, inventory mutations, anything where "we told the user it succeeded" must be true: use `w: majority`. High-volume analytics events, activity logs, telemetry: `w: 1` is probably fine.

### Native sharding

MongoDB's sharded cluster has three components:

**`mongos`** is the query router. A stateless process that sits between your application and the shards. Applications connect to mongos as if it were a regular MongoDB instance. mongos holds a cached view of which shard owns which chunk of the key space and routes operations accordingly.

**Config servers** are a 3-node replica set that stores the authoritative cluster metadata: which shard key ranges map to which shards, which chunks have been migrated, and the overall topology. mongos reads this at startup and caches it.

**Shard replica sets** are individual replica sets that each own a portion of the data. Each shard is independently a fully functional replica set with its own primary, secondaries, WiredTiger storage, and oplog.

The **shard key** is the most consequential choice in a MongoDB sharded cluster. For `orders`, `order_id` is a reasonable shard key as UUID values hash uniformly across the key space, distributing writes across shards evenly. `status` would be a terrible shard key. There are four status values. MongoDB would create chunks for those four values and they'd end up on four shards. The overwhelming majority of new orders arrive as `pending` so essentially all new writes would hit one shard, defeating the entire purpose of horizontal scaling. This is a write `hotspot`, and it's a real production incident pattern.

### Cross-shard transactions

MongoDB added multi-document, multi-collection transactions in 4.0, extended to sharded clusters in 4.2. They use 2PC internally (same mechanism as Citus). Two network round trips to all involved shards, locks held during the prepare phase, coordinator failure complexity.

MongoDB's documentation is direct about the recommendation: **avoid cross-shard transactions where possible**. The idiomatic alternative is to model your data so that atomic operations touch a single document or a single shard. Embed related data in one document rather than referencing across documents. Ensure that operations that need to be atomic share the same shard key so they land on the same shard.

This is the same philosophy as Cassandra's table-per-query model as such move complexity to the data model so the engine doesn't have to coordinate at runtime. MongoDB gives you more flexibility in when and how you apply this principle. Cassandra enforces it structurally.

### CAP position

MongoDB leans CP when using majority read/write concerns. But MongoDB makes the CP/AP tradeoff configurable per operation via write concern and read preference.

With `readPreference: secondary` and `w: 1`, MongoDB shifts toward **AP** — reads may be stale (eventual consistency from secondary reads) and writes acknowledge quickly without waiting for durability guarantees. With `w: majority` and `readPreference: primary`, it's firmly CP. The application controls this dial, per operation if needed.

This flexibility is genuinely useful. A single MongoDB cluster can serve both a user-facing write path (strong consistency, `w: majority`) and an analytics read path (eventual consistency, secondary reads) without running two different database systems.

---

## Cassandra Multi-Node

### Designed for distribution from day one

If PostgreSQL's multi-node story is "single-node database plus tooling," and MongoDB's is "single-node engine with native replication and sharding added early," Cassandra's is different: **the distributed case is the design case**. Cassandra was built for multi-datacenter deployments at massive scale. The single-node behavior from Posts 5 and 6 already anticipates the cluster: partition keys, token ranges, and consistent hashing are in the single-node design because they were always intended for a ring of nodes.

### The ring and consistent hashing

Picture a circle representing the full range of possible Murmur3 hash values roughly -2⁶³ to +2⁶³, arranged in a ring. Every node in the cluster owns one or more **token ranges** contiguous arcs of that ring. When an `order_id` arrives, Cassandra hashes it to produce a token, and the node that owns the token range containing that value handles the write.

Every node has a complete copy of the ring topology which nodes own which token ranges. When a write arrives at any node, that node can independently compute which nodes are responsible for the target partition, without consulting any central authority. There is no config server, no fixed coordinator node; any node can act as coordinator per request, no routing metadata bottleneck.

**Virtual nodes (vnodes)** are why this works smoothly as the cluster changes. In a naive consistent hashing setup, each node owns one large arc. Adding a node means one neighbor transfers half its data. Only two nodes participate in the transfer, it serializes through one data transfer channel, and it takes a long time. With vnodes, each physical node owns many small, non-contiguous token ranges, typically 16 or more, scattered around the ring. When a new node joins, it takes a small fraction of vnodes from many existing nodes. The data transfer is parallelized across the entire cluster simultaneously. Adding a node is faster and less disruptive. The same applies to node removal: a failed node's vnodes were spread across many locations on the ring, so the failover load distributes across many surviving nodes instead of concentrating on two neighbors.

### Leaderless replication

Here is the most fundamental architectural difference from both PostgreSQL and MongoDB: **Cassandra has no primary for any partition**. All replica nodes are equal. Any node can accept a write or serve a read for any partition.

When a write arrives, whichever node received it becomes the **coordinator** for that operation, not permanently, just for this request. The coordinator hashes the partition key, consults its ring topology, identifies the replica nodes for that token range (the `N` nodes clockwise from the token's position, where `N` is the replication factor), and sends the write to all replica nodes **simultaneously**.

With a replication factor of 3 (`RF=3`), three nodes receive every write. None is the "primary" - all three are authoritative replicas. The coordinator waits for acknowledgment from however many replicas the consistency level requires, then confirms to the client.

Contrast this with PostgreSQL, where all writes go to one primary, and MongoDB, where all writes go to the primary of the relevant shard. In Cassandra, any node can absorb any write. There's no primary bottleneck, no election needed when a node fails, no concept of failover in the traditional sense. If one of three replicas is down, the coordinator writes to the other two (at QUORUM) and stores a hint for the missing replica.

### Tunable consistency

Cassandra's consistency levels determine how many replica nodes must respond before an operation is considered successful:

**`ONE`**: the coordinator sends the operation to all replicas but waits for only one to respond. Fastest. If the one responding replica has stale data (because it missed a recent write that went to other replicas), you get stale data back. Acceptable for workloads where eventual consistency is fine like analytics dashboards, activity feeds, non-critical reads.

**`QUORUM`**: the coordinator waits for a majority of replicas (2 of 3 for RF=3) to respond. The critical property: if you write at QUORUM and read at QUORUM, the two quorums must overlap by at least one node. That node definitely has the latest write. QUORUM reads and QUORUM writes together guarantee strong consistency so you always read the latest write. This is Cassandra's path to the consistency guarantees PostgreSQL provides by default, at some latency cost.

**`ALL`**: all replicas must respond. Maximum consistency, maximum latency, zero tolerance for replica unavailability. Rarely used in production.

**Hinted handoff** handles temporary replica unavailability. If a replica node is down when a write arrives, the coordinator stores a small record, a "hint", locally. When the missing replica comes back online, the coordinator replays the hint, bringing it up to date. This is why Cassandra maintains eventual consistency even when nodes are temporarily unreachable: writes are not lost, they're queued. The hint window has a limit. If a node is down for longer than the hint retention period (default 3 hours), hints are discarded and the node must be repaired via full anti-entropy repair.

### Cross-node transactions

Cassandra has no multi-partition transactions. The architectural reason is structural: with leaderless replication and tunable consistency across a ring, coordinating a distributed lock across multiple partition owners would require knowing which nodes own each partition, acquiring locks on all of them, running a commit protocol that respects each partition's consistency level, and handling partial failures in a system where there's no single coordinator to drive the protocol. That coordination overhead is fundamentally at odds with the design goals — it would serialize writes and introduce the kind of bottlenecks Cassandra was built to eliminate.

**Lightweight Transactions (LWT)** are the limited exception. LWT provides compare-and-set semantics for a single partition using Paxos (a distributed consensus algorithm). The intuition: "only insert this order if no order with this `order_id` already exists." Paxos runs four round trips across the replica set to complete a single compare-and-set operation. It's approximately 4× the latency of a regular write. Use it for the specific cases where idempotency or conditional writes are required.

Everything that PostgreSQL handles automatically with `BEGIN/COMMIT` - multi-table atomicity, rollback on failure, consistent view across multiple writes, the application must handle explicitly with Cassandra. Idempotent writes (so retries are safe), multi-table consistency via eventual repair or application-level checks, and acceptance that "all or nothing" across partition keys is not available.

---

## The Full Picture: Side-by-Side

### Write scalability

Cassandra scales writes most naturally. Adding nodes to the ring immediately increases write capacity as the new node takes vnodes from existing nodes and starts absorbing its share of writes. The append-only LSM write path means each node's write throughput stays consistent as the cluster grows. There's no primary bottleneck, no coordinator saturation. Write capacity scales nearly linearly with node count.

MongoDB scales writes horizontally via sharding, but requires deliberate shard key selection and carries chunk migration and mongos routing overhead. With a good shard key and reasonable data distribution, MongoDB scales writes well. The operational complexity of managing shard key selection, chunk splits, and migration is real but manageable, especially with MongoDB Atlas.

PostgreSQL's write scaling story is the hardest of the three. A single primary handles all writes, and that ceiling is bounded by one machine's throughput. Citus adds horizontal write scaling, but multi-shard transactions, coordinator bottlenecks, and shard rebalancing complexity make this a meaningfully different operational profile from native clustered systems.

### Read scalability

All three databases scale reads horizontally. PostgreSQL does it with read replicas. MongoDB does it with secondary reads, with read preference giving the application control over the consistency/latency tradeoff per operation. Cassandra distributes reads across the ring so any replica can serve a read, and the consistency level determines how many must respond.

The difference is in what "a read" costs. A PostgreSQL read on a standby replica is a B+Tree traversal to a heap fetch the same cost as on the primary. A MongoDB secondary read is the same WiredTiger B-Tree lookup. A Cassandra read at QUORUM contacts 2 of 3 replicas and returns the one with the highest timestamp network overhead, but no central bottleneck. For latency-sensitive reads, Cassandra's nearest-replica routing (consistency ONE) can produce lower read latency than a PostgreSQL read that must always go to the primary or a designated replica.

### Consistency

PostgreSQL's default behavior is strong consistency on the primary and eventual consistency on standbys (due to replication lag). Synchronous replication makes standbys strongly consistent at write latency cost. There's no per-query consistency dial, it's a cluster configuration.

MongoDB's consistency is per-operation configurable via write concern and read preference. `w: majority` + `readPreference: primary` gives you strong consistency. `w: 1` + `readPreference: secondary` gives you eventual consistency with lower latency. The spectrum is accessible to the application without changing cluster configuration.

Cassandra defaults to eventual consistency (write ONE / read ONE) and can provide strong consistency via QUORUM/QUORUM. The per-query nature of consistency levels is Cassandra's most operationally unique feature. you can be strongly consistent for critical writes and eventually consistent for analytical reads within the same cluster, the same table, even the same application session.

### Cross-node transactions

PostgreSQL: full ACID on a single primary, 2PC across Citus shards with real performance cost. The richest transaction semantics of the three.

MongoDB: multi-document ACID transactions available, cross-shard via 2PC, with strong guidance to avoid cross-shard operations through data modeling. Single-document operations are always atomic.

Cassandra: no multi-partition transactions. LWT for single-partition conditional writes via Paxos. Application owns cross-partition consistency. The most constrained of the three, by design.

### Data modeling flexibility

PostgreSQL is the most flexible. Add a column, add an index, run arbitrary SQL queries. Schema evolution is well-tooled and access patterns can change without re-architecting the data model.

MongoDB is next. Document flexibility allows schema evolution without migrations. Secondary indexes and the aggregation pipeline support diverse query patterns. Changing access patterns is cheaper in MongoDB than in Cassandra because you're adding indexes, not rewriting the data model.

Cassandra is the least flexible. Adding a new access pattern means adding a new table and backfilling it with existing data. Backfilling 1 billion rows for a new access pattern is an operational event, not a schema migration. This cost is acceptable when access patterns are known and stable. It's expensive when they're not.

---

## The Decision Framework

### 1. Do you need ACID transactions across multiple rows or tables?

If the answer is **yes, non-negotiably** like for financial transactions, inventory mutations, any operation where "partial success" is unacceptable then PostgreSQL is the safe choice. Its ACID guarantees are native, battle-tested, and operate without 2PC overhead on a single primary. MongoDB at `w: majority` with single-document operations or careful embedded data modeling comes close. 

If the answer is **yes, but you can model around it** then MongoDB's document model often lets you embed related data in a single document, making the "transaction" a single atomic document write. This works well when the data that needs atomicity naturally belongs together.

If the answer is **no, or only within a single partition** then all three are viable.

### 2. What is your write-to-read ratio and how will it grow?

**Read-heavy with complex queries** - `JOIN` across tables, range scans on multiple columns, ad-hoc aggregations: PostgreSQL. Its B+Tree structure, linked leaf nodes, and query planner are optimized for exactly this. MongoDB is a reasonable second choice if your data is document-shaped and queries are predictable.

**Write-heavy with known access patterns** - millions of inserts per minute, time-series data, event ingestion, activity logs: Cassandra. The benchmark numbers from Post 6 aren't theoretical. At 8 concurrent threads, Cassandra wrote `118,000 rows/sec` vs PostgreSQL's `31,000`. Under sustained write load, that gap widens further.

**Write-heavy with unknown or evolving access patterns** - this is the trap. Cassandra's write throughput is compelling, but if your access patterns aren't stable, you'll be adding tables and backfilling data constantly. MongoDB handles this better: strong write performance with the ability to add indexes as new query patterns emerge.

### 3. What consistency does your application actually need?

**Strong consistency is required** for financial account balances, inventory counts, authentication state, any domain where two concurrent reads returning different values causes a real problem. PostgreSQL. MongoDB at `w: majority`. Cassandra at QUORUM/QUORUM can achieve this but at latency cost and with the constraint that it applies per-partition, not across multiple partitions atomically.

**Eventual consistency is acceptable** for social activity feeds, analytics dashboards, recommendation systems, logging, IoT sensor data, notification systems - any domain where "the user sees data from 200ms ago" is a non-event. Cassandra's AP model is a feature for these workloads, not a limitation. You get higher availability, lower write latency, and simpler failure behavior in exchange for accepting that reads may occasionally be slightly stale.

---

## The Honest Default

If you are building a new product and do not know your scale yet: **start with PostgreSQL**.

PostgreSQL is the most flexible, the most forgiving of evolving requirements, the easiest to hire for, and the most straightforward to operate. When you understand your actual bottlenecks or when you've run the benchmark against your data and real access patterns and the numbers tell you that you need something different - that's when you migrate.

---

## End of Series

That's the series. 7 posts from "what happens when you call INSERT" to "how do you choose between three databases at production scale." If you've read all, you now understand what most engineers who use these databases every day have never had to think about.