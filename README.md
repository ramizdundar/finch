# Finch

Finch is a minimalistic distributed in-memory key-value store written in C++.

# Table of Contents
- [Installation & Running](#installation--running)
- [Testing](#testing)
- [Design Overview](#design-overview)
    - [Example Dataflow](#example-dataflow)
- [Design Choices](#design-choices)
- [LLM Usage](#llm-usage)
    - [LLM Usage in Production](#llm-usage-in-production)

## Installation & Running

First, clone the Finch repository:
```
git clone https://github.com/ramizdundar/finch.git
```

Navigate to the project folder and compile:
```
cd finch
mkdir -p build
cd build
cmake ..
make
```

You can now run as many servers as you'd like:
```
./server
```

Copy `node_list.txt` into the `build/` directory (next to the client binary). By
default, `node_list.txt` contains three localhost addresses, assuming they’re
available. This file should ideally list the addresses of the servers you’re
running:
```
cp ../node_list.txt .
```

Finally, start the client:
```
./client
```

At this point, you can explore the main functionality of the client, integrate
your own code, or check out the tests.

## Testing

Although testing wasn't a requirement, ensuring a working implementation was.
Therefore, Finch includes a minimal benchmarking and stress testing utility to
verify its functionality. If you've followed the installation steps, no
additional setup is required.

To run the test:
```
./test
```

By default, the test creates 10 threads, each performing 100,000 random
operations, for a total of 1 million operations. The operations are split as
follows: 40% get, 40% put, and 20% del. It verifies the correctness of the
operations using a local map. At the end of the test, it cleans up any remaining
entries and checks their values for accuracy.

- Key length: Uniformly distributed between 5 and 15 characters
- Value length: Uniformly distributed between 5 and 50 characters

All of these parameters are configurable in `test.cpp` but require recompiling
the project after changes.

## Design Overview

Finch consists of two components: the client and the server. Multiple servers
can coexist on the same machine or across different machines. Clients discover
available servers through a file named `node_list.txt`, which contains a
newline-separated list of server addresses. This file must reside in the same
directory as the client binary.

When a client is initialized, it attempts to connect to all the servers listed
in `node_list.txt` and keeps these connections alive. This design ensures that
requests can be sent to the correct server with minimal latency. The server
responsible for handling a particular request is determined by taking the modulo
of the key's hash value with the number of available servers.

The client API:
```
std::string get(const std::string& key);
bool put(const std::string& key, const std::string& value);
bool del(const std::string& key);
```

Message Structure:
```
+-------------------+
| Total Size (N)    | (4 bytes, uint32_t)
+-------------------+
| Operation Type    | (1 byte, uint8_t)
+-------------------+
| Key Hash (H)      | (8 bytes, uint64_t)
+-------------------+
| Key Length (L)    | (4 bytes, uint32_t)
+-------------------+
| Key (K)           | (L bytes)
+-------------------+
| Value Length (VL) | (4 bytes, uint32_t) [Only for SET operation]
+-------------------+
| Value (V)         | (VL bytes) [Only for SET operation]
+-------------------+
```

Each server is divided into 1024 partitions by default, with each partition
implemented as a `std::unordered_map` and a `std::mutex`. The server listens on
port 12345 by default, incrementing the port number if the default is already in
use. When a client connects to a server, a dedicated thread is spawned to handle
all requests from that client.

Each partition operates independently, and requests are routed to the
appropriate partition based on the key's hash value. The partition count of 1024
applies to each server instance, meaning that if there are 3 servers, the system
will have 3072 partitions in total.

When a client sends a request to a server, the corresponding partition is locked
while the operation (GET, PUT, or DELETE) is performed. This ensures consistency
while maintaining high performance across multiple partitions and servers.

### Example Dataflow

Let’s walk through the following code snippet to understand how Finch works.
Before running this code, we assume that the servers are already up and running,
and `node_list.txt` contains the correct server addresses:

```
FinchClient client;
if (client.put("mykey", "myvalue")) {
    std::cout << "Key stored successfully.\n";
}
```

1. When the `FinchClient` is created, the `node_list.txt` file is parsed, and
   the client establishes connections to all listed servers.
1. Each server creates a dedicated thread to handle the connection from the
   client.
1. During the put operation, the client first checks if the connection to the
   server is still alive. If not, it attempts to reconnect.
1.  A message is constructed according to the structure described earlier, which
    includes the operation type, key, and value.
1. The server thread receives the message and, based on the key's hash value,
   locates the appropriate partition.
1. The partition's mutex is locked to ensure thread-safe access, and the
   operation (in this case, storing the key-value pair) is performed on the
   underlying map.
1. Once the operation is complete, the server thread creates a response and
   sends it back to the client, confirming the success of the operation.

## Design Choices

After reviewing the requirements, a few key points stood out:

- **The simplest** distributed key-value store.
- Use of POSIX.
- This test is about prioritizing shortcuts and decisions.
- A plus if the solution is implemented in C/C++.

I quickly ruled out some features:

- Encryption
- Authentication
- Serialization (I assumed keys and values are sequences of bytes).

After further consideration, I also discarded the following, as they don’t fit
within a 2-hour implementation window and don’t align with the “simplest”
requirement:

- Persistence
- Fault tolerance

By removing these features, I was left with a barebones system, which led to
Finch. I also imposed a constraint to only use the standard library for now,
excluding external libraries like Boost. Below is a Q&A format explaining my
remaining design decisions.

Q&A on Design Decisions: 

**Q: Why are keys and values strings?** 

> I wanted to support keys and values of arbitrary lengths. I originally looked
for something equivalent to byte[] in Go within C/C++ and found options like
char*, char[], or void*. I opted for std::string due to its simplicity and ease
of use.

**Q: Have you considered fixed-size keys and values?** 

> Yes, I considered using fixed-size keys and values to prevent fragmentation,
as this could be an issue initially since I was not planning to implement or
modify the allocator. However, Finch does not need to address such problems at
this stage, as it has more pressing concerns to focus on.

**Q: Why did you choose blocking I/O?** 

> Blocking I/O with read() and recv() performs well for a manageable number of
connections. While select() and poll() provide I/O multiplexing, they don't
scale efficiently with a large number of file descriptors, as they require
managing callbacks for each fd during the call and collecting results after.
Given these trade-offs, simple blocking I/O is more straightforward and
sufficient for this case.

**Q: Why not use a thread pool?** 

> A thread pool only adds complexity without I/O multiplexing. Since the clients
don’t frequently close their connections, I would either (a) need to manage
non-blocking recv() calls or (b) dynamically extend the thread pool for incoming
clients. Given that client events (starts and ends) are infrequent and the
number of connections isn't excessively high, individual threads suffice for
this use case.

**Q: Why did I assume number of clients can't be excessively high?**

> Based on discussions with Santana, my focus in this role is more about
handling high data volumes rather than managing a large number of concurrent
clients. Therefore, optimizing for high-volume, low-connection scenarios is more
sensible than designing for numerous low-volume connections.

**Q: Why no replication?** 

> Replication typically serves two purposes: (1) fault tolerance and
availability, and (2) scaling read requests. Since I excluded fault tolerance
and the system is in-memory, scaling reads doesn’t offer much benefit. If disk
storage were involved, scaling reads could be more worthwhile.

**Q: Why no disk usage or WAL (Write-Ahead Logging)?**

> While a Redis-like WAL could be feasible, implementing compaction—especially
concurrent compaction—is complex and not achievable within 2 hours. More
advanced structures like LSM trees or B+ trees were also out of scope for this
project.

**Q: Why partition the data if there’s no replication?** 

> Partitioning is mainly used here to reduce lock contention. Without
partitioning, contention on a single lock would increase as client operations
grow.

**Q: Why no read-write locks?**

> If the standard library offered them, I would have used them. While I could
have explored an external source or LLM suggestions, I avoided them to maintain
simplicity. Additionally, partitioning isn’t ideal in this setup. Ideally, the
partitions should dynamically split as they grow, which would enter concurrent
hash map territory. In the future, using a concurrent hash map backend would be
a more scalable solution. I avoided Boost’s concurrent_flat_map due to the
decision to stick with no external dependencies for now.

**Q: Why no cluster discovery?**

> While cluster discovery can be useful for scaling up and down implementing it
properly would require some form of consensus, either through a custom
solution—which is beyond the scope of this project—or by using a tool like etcd,
which would add dependencies. Alternatively, we could assume no faults and use a
simpler approach like multicast for discovery. I believe this could work, but I
had already invested more than 2 hours in the current implementation, so I
decided not to include it.

**Q: Why no consistent hashing?**

> Consistent hashing isn’t necessary because we’re not planning to scale up or
down, either intentionally or due to failure. Without these scenarios, the added
complexity of consistent hashing isn’t justified.

**Q: Have you considered other designs?**

> Initially, I thought about introducing fault tolerance by using a multicast
setup without a master-slave architecture to avoid complexity of master
election. The idea was for each node to maintain its own cluster view while
avoiding duplicate partition ownership in the event of a failure. However,
managing nodes rejoining the cluster introduced too many edge cases, making the
solution overly complex, so I decided against it.

> I also explored a lock-free approach where each map would be owned by a
separate thread, with communication between client-handling server threads
through SPSC queues. However, I reconsidered this design because I wasn't sure
if the locking overhead would outweigh the cost of frequent context switching in
this approach.

> I briefly considered a single-threaded design using poll() for I/O management.
While it’s simple, I ultimately chose the current implementation because it
scales better with equal amount of servers. If I were to go the single-threaded
route, I would prefer an epoll/io_uring thread-per-core, shared-nothing
architecture, similar to systems like DragonflyDB, Redpanda, and ScyllaDB.

## LLM Usage

All code generation LLM prompts are here:

https://chatgpt.com/share/670c2b28-678c-8010-ae69-1d6890587c9d

Starting prompt:
```
I need a simple distributed key value store called finch in cpp:
- it consists of 2 files client and server and 1 txt file node_list.txt
- data is partitioned across servers and each server has 1024 partitions
- client does hash % server_count to find which server t send
- server does hash % partition_count tp fin which partition to put
- each partition is unordered map
- server handle each client on a different thread after accepting
- this means scoped_lock is necessary to access partition, so each partition has a mutex
- servers clsiten on 12345 if that is occupied 12346 and so on..
- client supports get, put and del
- client reads server list from node_list.txt
- both client an server only use posix send() recv()
```

Few examples:
```
I decided to change client API, it no longer interacts with iss and has this 3 methods:
std::string get(const std::string& key);
bool put(const std::string& key, const std::string& value);
bool del(const std::string& key);

Now I want you to write a test.cpp which creates N clients across N threads and 
does random 1M op and check if the values are as expected

test.cpp is faulty because keys can overlap. For example a random key from 
client0 and 1 may be the same. To solve this prepend 0 to client 0 1 to 
client 1 etc.

modify client code so it doesnt create a new connection each time. It holds the 
connection as long as client is alive, it reconnects only if connection isn't alive
```

Besides these, I used LLMs extensively for design and other purposes. For
example:

Learning about cpp:
```
can you list me common cpp stl data structures and common operation 
complexities?

i have a vector in main that holds other vectors. Inside one of my threads I 
create a vector and put that in my main vector, then my thread ends. At this 
point if main tries to access the newly added vector, what will happen?

how can I pass the ownership of vector from one thread to another cpp?

what is the best config file for cpp code?

How does default allocator of C++ work for example when I do new does it try 
to find the first available? Space and put it there or does it do something 
different?
```

Distributed systems:
```
how many nodes are good enough for 4 node cluster consistent hashing to make 
distribution roughly equal?

in a distributed cluster can you give me a summary of ways to do discovery
```

I/O options in POSIX:
```
what are the POSIX IO options?

what do you think would be the ideal way to store the values in this key value 
store using only POSIX APIs?

How does POSIX AIO compares to io uring for filesystem read/write and why the 
need for latter if the prior doesn't lack anything?

> In many operating systems, including macOS and Linux, the actual 
implementation of POSIX AIO is suboptimal for general file I/O. It often 
defaults to a thread-based implementation underneath, meaning that the kernel 
spawns threads to handle these asynchronous operations, which is not as 
efficient as truly asynchronous I/O.

If this is the case, would carefully tuned pool for blocking IO perform better 
that posix AIO? Since both spawn threads?
```

Design Doc:
```
Can you polish this design document?

can you generate a ascii diagram for view?
|
 ->  add them side to side
    |
     -> can you add them below each other?

can you generate me an ascii art of finch?
```

### LLM Usage in Production

In production, I anticipate that the use of LLMs will be similar to how I've
utilized them in this project, with code generation being more focused on
refinement rather than initial creation.

Based on my experience, I primarily benefit from LLMs in three ways:

1. Polishing Writing: Whether it's comments, Slack messages, or issue
   descriptions, LLMs excel at refining text. I typically draft the initial
   version myself and then ask the LLM to polish it. This allows me to
   selectively incorporate changes or further tweak the LLM’s suggestions.
1. Code Generation and/or Refinement: The most challenging aspect of coding is
   the design process, not the actual writing. Once I have a clear idea, I use
   LLMs to generate code function by function, which simplifies the review
   process. Or, I more often use LLMs to generate comments or polish existing
   code.
1. Enhanced Research: I find myself relying less on Google and more on LLMs for
   targeted queries or guidance. I frequently ask ChatGPT directly or use Google
   only to search specific websites. For example, understanding that POSIX
   asynchronous file I/O is implemented using a thread pool in the
   kernel—meaning it’s not truly asynchronous
   ([source](https://lwn.net/Articles/671797/))—would have been much more
   challenging without ChatGPT’s assistance. While there is always a risk of
   hallucination, increased productivity makes occasional fact-checking
   worthwhile.

Another benefit is using LLMs to understand code, especially when it requires
domain expertise (such as bizzare bitwise operations, rare design patterns,
etc.), involves poorly written code (like functions passed as arguments deep in
the call stack without static typing), or when trying to comprehend error
messages (especially in C++). I believe this advantage will become increasingly
important as LLMs become more affordable and can handle larger datasets more
easily, such as processing entire repository folders as input.

Cons and Considerations When Using LLMs:

1. Uniform Writing Style: While I don’t mind ChatGPT's writing style, it tends
   to generate very similar responses to similar queries, and conversational
   history usually isn't very influential. For instance, if you polish a
   paragraph, fix two sentences, and then provide the revised paragraph with new
   sentences for further polishing, ChatGPT may revert to the original version
   instead of focusing on the newly added sentences.
1. Need for Specificity: If your requests aren't very specific, the generated
   content can often be disappointing. Consequently, it's often faster to write
   your own content and then use LLMs for polishing rather than repeatedly
   generating content from the LLM that requires revision.
