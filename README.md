# Custom Network File System (NFS)

## Architecture Status & Marks Tracking

| Spec Item | Feature Description | Status |
| :--- | :--- | :--- |
| **1.1** | Initialization (NM + Threaded Multi-SS Registration) | ✅ Completed |
| **1.2** | SS Functionalities (Read/Write/Info/Stream) | ✅ Completed |
| **1.3** | NM Functionalities (Directory Database Storage) | ✅ Completed |
| **2.0** | Client Operations (CRUD, Audio Stream, Path Finding) | ✅ Completed |
| **3.1** | Synchronous Baseline Writing Implementation | ✅ Completed |
| **3.2** | Read/Write Exclusivity Locking Logic Engine | ✅ Completed |
| **3.3** | Explicit Error Codes | ✅ Active |
| **3.4** | Trie Search Optimization & LRU Cache System | ❌ Up Next (Day 4) |

## Day 2 Execution Validation Pass

### 1. Recompile Code Base
```
gcc naming_server.c -o naming_server -lpthread
gcc storage_server.c -o storage_server -lpthread
gcc client.c -o client

```

### 2. Run Components

Terminal 1 (Naming Server):

```
./naming_server 8080

```

Terminal 2 (Storage Server - Launch with dummy local path artifacts):

```
touch sample.txt
./storage_server 192.168.1.5 8080 9090 9091 sample.txt

```

Terminal 3 (Client Data Reading Pass):

```
./client 192.168.1.5 8080 READ sample.txt

```
