# Implementation of BitTorrent Protocol

## Overview
The **BitTorrent protocol** is a decentralized peer-to-peer (P2P) file-sharing protocol designed to distribute large files efficiently over the internet. It allows peers to simultaneously upload and download file segments, thereby reducing the reliance on a central server. Files are divided into segments, which are shared among peers participating in a "swarm".

This project implements the BitTorrent protocol, focusing on both the client-side logic and the tracker functionality.

---

## Logic of the Code

### 1. Peer Initialization
- Each peer parses its input file and sends the tracker a list of owned files along with all the corresponding segments.
- The peer waits for an acknowledgment (ACK) message from the tracker before requesting the desired files from other peers.

### 2. Client Requests for Wanted Files
- Once the client receives an ACK message, it starts requesting the desired files one by one.
- For each requested file, the client:
  - Sends the file name to the tracker and waits for a response.
  - The tracker responds with:
    1. All the hashes of the file.
    2. A list of all available seeders for the file.
- The client employs a **Round-Robin algorithm** to select a seeder, ensuring balanced requests.
- After receiving a segment, the client:
  - Verifies if the received segment matches the expected one.
  - If not, the client requests the segment from the next seeder in the list until the correct segment is received.
  - If the segment is correct, the client adds it to its local `owned_files_by_peer` list.
- After receiving 10 segments, the client requests an updated list of seeders from the tracker.
- Once all segments of the file are received:
  - The client reconstructs the file and writes it to the output.
- After obtaining all requested files, the client notifies the tracker that it has completed its tasks.

### 3. Client Response to File Requests
- A dedicated upload thread runs in an infinite loop, listening for segment requests.
- When a segment request is received:
  - If the segment is available locally, the client sends it to the requesting peer.
  - If the segment is unavailable, the client sends a NACK (negative acknowledgment) message.

---

## Tracker Logic

 - The tracker coordinates file distribution among clients and handles various types of requests in a loop.
 - The loop terminates when all clients have received their requested files.

### Tracker Request Types
1. **INIT_MSG_FROM_PEER**:
   - The tracker receives a list of files owned by the peer and creates a swarm for each file.

2. **FILE_REQUEST_MSG**:
   - The tracker receives the name of the requested file.
   - Sends the requesting peer a list of current seeders for the file.
   - Marks the requesting peer as a seeder for the file, making it available to other peers.

3. **RESEND_SEEDERS_MSG**:
   - Sends an updated list of seeders for a specific file to the requesting peer.

4. **RECEIVED_ALL_FILES**:
   - Increments a counter tracking how many clients have completed their tasks.

### Finalization
- When the tracker receives a `RECEIVED_ALL_FILES` signal from all clients, it sends a signal to all clients, instructing them to close their upload threads and terminate the process.

---