# Network File System

## Assumptions:
- Both writers and readers cannot access the same file simultaneously; only one of them can do so. Writers may starve while waiting for the readers or writers to complete its task.

- We assumed that storage servers are always connected throughout the process of copying its contents into a backup storage servers.

- We assumed all the files in the directory are accesible but not the directories inside them unless specified as an accessible path.

- We assumed all port numbers to be unique storage servers.

- We assumed that initial ACK should be recieved within timeframe of one second. 
