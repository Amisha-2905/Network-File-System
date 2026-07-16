# Network File System (NFS) - A From-Scratch Replica of Hadoop-Style Distributed Storage

A high-performance, fault-tolerant, and concurrency-aware distributed file system implemented entirely in C from scratch. This project is a simplified replica of Hadoop-style distributed storage architecture, modeled on the core ideas behind HDFS: a metadata-driven naming layer, distributed storage nodes, replication for availability, and recovery mechanisms for node failures.

Rather than relying on existing distributed storage libraries or frameworks, every core component was implemented manually using TCP sockets, POSIX threads, a custom request/response protocol, and a custom metadata/data separation architecture. The goal was to understand, from the ground up, how distributed systems handle data placement, naming, locking, failure recovery, and concurrency.

---

## Why This Project Matters

This project was built to go beyond basic networking and file handling. It was designed to explore the real engineering challenges behind large-scale distributed systems:

- How to separate metadata from data to avoid bottlenecks
- How to ensure correct behavior when multiple clients access the same files concurrently
- How to replicate data to improve reliability and availability
- How to detect failures and recover safely without corrupting system state
- How to build a system that remains correct under partial failure and timing uncertainty

In short, this project is not just about making files available over the network. It is about building a system that behaves like a real distributed storage platform under stress.

---

## What This Project Replicates from Hadoop-Style Design

This implementation is inspired by the architecture and principles of Hadoop’s distributed storage model, especially the concepts behind HDFS:

- NameNode-like metadata management through a Naming Server
- DataNode-like storage through multiple Storage Servers
- Distributed file placement and replication
- Metadata-driven request routing
- Fault tolerance and recovery mechanisms

Although it is a simplified academic replica rather than a production-grade Hadoop deployment, it captures the same core ideas that make distributed storage systems scalable, fault-tolerant, and practical.

---

## System Overview

The project consists of three main components:

1. Naming Server (NM)
   - Acts as the metadata/control plane
   - Tracks files, directories, and their locations
   - Routes client requests to the correct storage node
   - Maintains server health and recovery state

2. Storage Servers (SS)
   - Act as the data plane
   - Store file data and serve read/write requests
   - Participate in replication and recovery workflows
   - Support both buffered async writes and synchronous writes

3. Client
   - Provides a command-line interface for interacting with the cluster
   - Sends requests such as create, read, write, delete, copy, and status queries

This separation of responsibilities reflects the same architectural spirit used in large-scale distributed file systems.

---

## 1. Naming Server (NM)

The Naming Server is the core control component of the system. It is responsible for maintaining metadata, serving lookups, and coordinating the health and availability of storage nodes.

### Responsibilities
- Maintains a global view of files and directories
- Resolves file paths to their storage locations
- Handles create, delete, read, and listing operations
- Tracks storage server status and liveness
- Coordinates replication and recovery workflows

### Technical Highlights
- Prefix Trie-based path organization
  - Paths are tokenized and stored hierarchically
  - Supports efficient routing and directory traversal
- LRU caching for hot paths
  - Frequently accessed metadata entries are cached to reduce repeated work
- Thread-per-connection handling
  - Each client or storage server connection is processed concurrently
- Thread-safe metadata management
  - Shared state is protected using synchronization mechanisms

### Why This Component Matters
In distributed systems, metadata services often become bottlenecks if not designed carefully. By optimizing lookup structure and handling concurrent requests carefully, the Naming Server remains responsive under load.

---

## 2. Storage Servers (SS)

The Storage Servers form the data layer of the system. They are responsible for storing file contents and serving read/write operations.

### Responsibilities
- Accept file data from clients
- Store files on disk
- Register with the Naming Server dynamically
- Support asynchronous buffered writes and synchronous durability writes
- Participate in replication and recovery flows

### Technical Highlights
- Dynamic registration
  - Storage servers can join the cluster at runtime
  - They discover their network-facing address dynamically rather than being hardcoded
- Buffered asynchronous write pipeline
  - Data can be acknowledged quickly while being flushed to disk in the background
- Optional synchronous writes
  - Supports stronger durability guarantees when needed
- Direct interaction with the client and naming server
  - Storage servers are active participants in cluster operations, not passive storage devices

### Why This Component Matters
The storage layer is where performance and durability are tested. This project implements a balance between fast response times and safe persistence.

---

## 3. Replication, Fault Tolerance, and Recovery

A distributed file system is only useful if it behaves correctly when failures happen. This project includes mechanisms to improve availability and maintain correctness under adverse conditions.

### Replication
- When the system reaches a certain storage-server threshold, data is replicated across multiple nodes
- Replication is handled in a way that does not block the primary client request unnecessarily
- The design supports continued availability even if one copy becomes inaccessible

### Heartbeat Monitoring
- The Naming Server periodically probes storage nodes
- Nodes that fail to respond are marked unhealthy
- Stale or invalid state tied to failed nodes is cleaned up

### Read Failover
- If a primary node becomes unavailable, reads can be redirected to an available replica
- This improves resilience and reduces the impact of single-node failure

### Recovery and Reconciliation
- When a previously failed server comes back online, the cluster handles the event as a recovery process rather than a fresh insertion
- The system avoids inconsistent metadata state and duplicate registrations
- Recovery logic is designed to preserve the integrity of the global file view

### Why This Matters
Real distributed systems are judged not by how they behave under ideal conditions, but by how they recover when things go wrong. This project places strong emphasis on that principle.

---

## Concurrency and Correctness

One of the biggest challenges in this project was building the system to be safe under concurrent access.

### Concurrency Mechanisms
- POSIX threads are used to handle multiple clients and server operations at once
- Shared metadata and file state are protected using synchronization primitives
- The design avoids race conditions that could corrupt the global state

### Correctness Challenges Addressed
- Preventing stale reads while writes are still in progress
- Protecting shared metadata during concurrent updates
- Ensuring asynchronous completion events are not lost
- Maintaining consistency during node recovery and re-registration

### Why This Matters
Many distributed systems bugs are not obvious at first glance. They arise only when threads, timing, and network behavior interact in subtle ways. This project was specifically designed to expose and solve those problems.

---

## Client Interface

The client provides a simple but functional command interface for interacting with the distributed file system.

### Supported Operations
- `READ <path>`: read file contents
- `STREAM <path>`: stream data through the client
- `WRITE <path> "<data>"`: write data to the cluster
- `WRITE <path> "<data>" --SYNC`: force synchronous write
- `CREATE <path>`: create a new file
- `DELETE <path>`: remove a file from the cluster
- `COPY <source> <dest>`: copy data between locations
- `INFO <path>`: retrieve metadata
- `LIST none`: list available paths
- `STATUS <id>`: query the status of an asynchronous operation

These commands make the system feel like a real distributed storage interface rather than a toy prototype.

---

## Build and Run

The project is built using GCC and POSIX threads on Linux.

### Compile
```
gcc naming_server.c -o naming_server -lpthread
gcc storage_server.c -o storage_server -lpthread
gcc client.c -o client
```

### Start the Naming Server
```
./naming_server <NM_Listening_Port>
```

Example:
```
./naming_server 8080
```

### Start Storage Servers
```
./storage_server <NM_IP> <NM_Port> <My_NM_Facing_Port> <My_Client_Facing_Port> [Accessible_Paths...]
```

Example:
```
./storage_server 192.168.1.5 8080 9090 9091 file1.txt file2.mp4
```

### Interact with the Client
```
./client <NM_IP> <NM_Port> <COMMAND> <PATH> [PAYLOAD / FLAGS]
```

Example:
```
./client 192.168.1.5 8080 WRITE /file1.txt "hello from the cluster"
```

> Important: For real network testing, use the actual LAN/Wi-Fi IP address of the machines involved. Do not rely on `127.0.0.1` for multi-machine deployment.

---

## Problems Faced

This was one of the most important and rewarding parts of the project. The real learning came not from implementing the happy path, but from diagnosing and solving issues that only appear under concurrency, failure, and timing uncertainty.

### 1. TCP Teardown Race During Asynchronous Completion

One of the earliest issues involved the asynchronous write completion path. After a storage server completed writing data to disk in a background thread, it attempted to notify the Naming Server with a completion message. The problem was that the connection could be closed too early, and the packet might be lost before the naming server successfully received it.

This produced a subtle bug where asynchronous operations remained stuck in a pending state indefinitely.

#### How It Was Solved
A handshake-based acknowledgment mechanism was introduced so that completion messages would only be considered delivered once the Naming Server had positively acknowledged them. This made the delivery process more reliable and prevented silent message loss.

#### Why It Matters
This taught me that distributed systems correctness depends not only on logic but also on transport-level behavior. A system may look correct in tests and still fail under real TCP timing conditions.

---

### 2. Race Conditions Between Concurrent Read and Write Operations

Another major challenge was handling concurrent operations targeting the same file. When a write and a read occurred at nearly the same time, the read path could sometimes observe inconsistent state because the write operation had not fully completed or the shared lock semantics were not enforced with enough strictness.

This exposed the difficulty of maintaining correctness in a system with multiple threads and asynchronous I/O.

#### How It Was Solved
The locking strategy was refined and tested under contention. The implementation was stress-tested with overlapping read/write operations to validate that shared state remained protected and that the system behaved deterministically under concurrency.

#### Why It Matters
This experience reinforced the importance of designing synchronization carefully, especially in systems where threads interact with shared files, shared metadata, and background I/O.

---

### 3. Safe Recovery of Storage Servers After Failure

A subtle but serious issue appeared when a storage server that had previously been marked dead came back online. A naive recovery approach could overwrite existing metadata, create duplicate registrations, or cause the Naming Server to treat the node as a fresh new server rather than a recovered one.

This could break the cluster’s internal consistency and make the metadata view unreliable.

#### How It Was Solved
The recovery workflow was redesigned so that previously known nodes were processed as recovery events. The system preserved valid metadata, reset stale state, and re-established the node’s role in a way that avoided duplication and inconsistency.

#### Why It Matters
This taught me that recovery is not a minor feature in distributed systems—it is a core correctness requirement. A system must be able to recover safely without breaking its global invariants.

---

### 4. Maintaining Consistency Under Partial Failures

The system also had to handle situations where a component was partially available: it could respond slowly, fail midway, or appear reachable while not fully healthy. These are common conditions in real distributed systems and are much harder to reason about than simple crash scenarios.

#### How It Was Solved
The implementation relied on explicit state transitions, heartbeat-based health checks, and conservative behavior around replication and completion events. The system avoided over-trusting transient success signals and instead focused on preserving correctness even under uncertain conditions.

#### Why It Matters
This project showed me that hard distributed systems engineering is mostly about handling uncertainty gracefully, not simply making the common case work.

---

## What I Learned

This project strengthened my understanding of several core systems concepts:

- Network programming and socket-based communication
- Thread synchronization and race prevention
- Metadata/data separation in distributed systems
- Replication and fault-tolerance design
- Recovery and consistency under failure
- Building scalable systems from first principles

It also gave me a much deeper appreciation for the importance of correctness, robustness, and careful protocol design in real-world distributed software.

---

## Feature Coverage and Specification Mapping

| Spec Item | Marks | Status |
| --- | ---: | --- |
| 1.1 Initialization | 60 | Done |
| 1.2 SS Functionalities | 120 | Done |
| 1.3 NM Functionalities | 30 | Done |
| 2. Client Operations | 80 | Done |
| 3.1 Async & Sync Writing | 50 | Done |
| 3.2 Multiple Clients | 70 | Done |
| 3.3 Error Codes | 20 | Done |
| 3.4 Search Optimization | 80 | Done |
| 3.5 Backup / Failure | 70 | Done |
| 3.6 Redundancy (Bonus) | 50 | Done |
| 3.7 Bookkeeping | 20 | Done |
| **Total Expected** | **650 / 650** | **Completed** |

---

## Final Note

This project represents a complete end-to-end distributed systems implementation built from scratch in C. It covers networking, concurrency, metadata management, data storage, replication, recovery, and client interaction in one unified system.

If you are looking for a project that demonstrates systems programming, distributed systems thinking, fault tolerance, and low-level engineering in a single portfolio piece, this is a strong example of that work.