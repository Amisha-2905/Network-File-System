# Custom Network File System (NFS)

A minimal, high-performance distributed file system built from scratch in C using POSIX threads and raw TCP sockets.

## Architecture Status & Marks Tracking

| Spec Item | Feature Description | Status |
| :--- | :--- | :--- |
| **1.1** | Initialization (NM + SS Registration) | Completed |
| **1.2** | SS Functionalities (Read/Write/Info/Stream) | Pending |
| **1.3** | NM Functionalities (Storage + Feedback) | Pending |
| **2.0** | Client Operations (CRUD, Copy, List) | Pending |
| **3.1** | Async / Sync Writing | Pending |
| **3.2** | Concurrent Multi-Client Handling | Pending |
| **3.3** | Explicit Error Codes | Framework Defined |
| **3.4** | Trie Search + LRU Cache | Pending |
| **3.5** | Data Replication & Fault Tolerance | Pending |
| **3.6** | **BONUS**: Storage Server Recovery | Pending |
| **3.7** | Bookkeeping & Live Logging | Function-Based Logger Built |

## Compilation & Execution Guide

### 1. Compile Code Base
```
gcc naming_server.c -o naming_server
gcc storage_server.c -o storage_server

```

### 2. Run Naming Server

Specify an open execution port for the central coordinator:

```
./naming_server 8080

```

*Note the dynamic network interface IP output to screen (e.g., `192.168.1.5`).*

### 3. Register a Storage Server

Provide the resolved NM network IP, target NM port, and a list of paths:

```
./storage_server 192.168.1.5 8080 9090 9091 text_dir/file1.txt media/audio.mp3

```