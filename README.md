# Distributed File System

A networked file system in C, built with sockets and POSIX threads. Several users can
create, read and edit shared text files at the same time, and every file is copied to
a second storage server so it survives a server going down.

Built for the Operating Systems and Networks course at IIIT Hyderabad (Monsoon 2025).
Ayush Sabhasad helped with the storage server.

## How it works

```
                 metadata, permissions
    Client  <--------------------------->  Name Server
       |                                        |
       | file data (READ, WRITE, STREAM)        | register, create, delete, sync
       v                                        v
    Storage Server (primary)  <-- copy -->  Storage Server (replica)
```

- The **name server** tracks files, folders, users and access rights. It finds a file
  through a hash map, logs every request, and saves its metadata to disk so that it
  can restart without losing track of anything.
- **Storage servers** hold the files and register with the name server when they start.
- The **client** asks the name server where a file is, then talks to that storage
  server directly. If the primary is down, it falls back to the replica.

When a server comes back, the name server synchronises its files with the copy that
stayed online.

## Build and run

```
make
./name_server                                    # listens on port 8000
./storage_server <nm_port> <client_port>         # start at least two
./client_user
```

For example, in separate terminals:

```
./storage_server 9101 9102
./storage_server 9103 9104
```

Everything runs on localhost by default. The client asks for a username when it starts.

## Commands

| Area | Commands |
|---|---|
| Files | `CREATE`, `READ`, `DELETE`, `INFO`, `VIEW`, `LIST`, `STREAM`, `EXEC` |
| Editing | `WRITE`, `ETIRW` (finish writing), `UNDO` |
| Folders | `CREATEFOLDER`, `VIEWFOLDER`, `MOVE` |
| Checkpoints | `CHECKPOINT`, `LISTCHECKPOINTS`, `VIEWCHECKPOINT`, `REVERT` |
| Access | `ADDACCESS`, `REMACCESS`, `REQUESTACCESS`, `VIEWREQUESTS`, `APPROVEREQUEST`, `DENYREQUEST` |

### Editing a file

Files are edited one sentence at a time, and that sentence is locked until you finish,
so two users can work on different sentences of the same file.

```
WRITE notes.txt 0
write> 0 hello world.
write> ETIRW
READ notes.txt
```

`WRITE <file> <sentence>` starts the edit. Each line after it is `<word_index> <text>`,
counting from 0.

### Folders

The root is `/`. Inside a folder, use the full path for every command.

```
CREATEFOLDER docs
CREATE docs/notes.txt
MOVE docs/notes.txt /
VIEWFOLDER docs
```

### Checkpoints

```
CHECKPOINT notes.txt v1
LISTCHECKPOINTS notes.txt
VIEWCHECKPOINT notes.txt v1
REVERT notes.txt v1
```

### Asking for access

```
REQUESTACCESS notes.txt READ        # or WRITE
VIEWREQUESTS notes.txt              # the owner sees who asked
APPROVEREQUEST notes.txt bob
DENYREQUEST notes.txt bob
```
