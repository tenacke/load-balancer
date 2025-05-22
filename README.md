# Load Balancer System

This is a simple load balancer system that consists of four main components:

1. **Watchdog**: Monitors and manages all processes, respawning them if they crash.
2. **Load Balancer**: Distributes client requests to reverse proxies using round-robin algorithm.
3. **Reverse Proxies**: Forward requests to backend servers.
4. **Servers**: Process requests and send responses back.

## Architecture

```
                     ┌──────────┐
                     │ Watchdog │
                     └────┬─────┘
                          │ (monitors all processes and respawns)
                          ▼
┌────────┐         ┌─────────────┐      ┌────┴─────┐
│        │         │             │      │          │
│ Client ├────────►│Load Balancer├─────►│ Reverse  │
│        │         │             │      │ Proxy 1  │─────────┐
└────────┘         └─────────────┘      └──────────┘         │
                          │                                  │
                          │             ┌──────────┐         │
                          └────────────►│ Reverse  │         │
                                        │ Proxy 2  │         │
                                        └──────┬───┘         │
                                               │             │
                          ┌────────────┬───────┼────┐        │
                          │            │            │        │
                          ▼            ▼            ▼        │
                     ┌──────────┐ ┌──────────┐ ┌──────────┐  │
                     │ Server 4 │ │ Server 5 │ │ Server 6 │  │
                     └──────────┘ └──────────┘ └──────────┘  │
                                                             │
                                           ┌─────────────────┘
                                           │
                          ┌────────────────┼──────────────┐
                          │                │              │
                          ▼                ▼              ▼
                     ┌──────────┐    ┌──────────┐    ┌──────────┐
                     │ Server 1 │    │ Server 2 │    │ Server 3 │
                     └──────────┘    └──────────┘    └──────────┘
```

## Components

### Watchdog

- Spawns and monitors all other components.
- Automatically restarts crashed processes.
- Handles termination signals and cleans up all processes.
- Uses pipes to receive port information from child processes.
- Sets environment variables for communication between processes.

### Load Balancer

- Listens on a fixed port 8080 (configurable).
- Distributes incoming connections to reverse proxies.
- Uses round-robin algorithm for load distribution.
- Gets reverse proxy port information from environment variables.

### Reverse Proxies

- Two instances running on dynamically allocated ports (typically 8081-8180).
- Forward requests to backend servers.
- Balance traffic to specific servers using random selection:
  - Reverse Proxy 1 routes to Servers 1, 2, and 3
  - Reverse Proxy 2 routes to Servers 4, 5, and 6
- Get server port information from environment variables.
- Report their chosen ports back to the watchdog.

### Servers

- Six worker instances on dynamically allocated ports (typically 9001-9006).
- Servers are organized into two groups:
  - Servers 1, 2, and 3 receive traffic from Reverse Proxy 1
  - Servers 4, 5, and 6 receive traffic from Reverse Proxy 2
- Process client requests.
- Generate responses.
- Report their chosen ports back to the watchdog.

## Dynamic Port Allocation

This system uses dynamic port allocation for reverse proxies and server processes:

1. **Load Balancer**: Uses a fixed port (8080 by default).
2. **Reverse Proxies**: Find available ports in the range 8081-8180.
3. **Servers**: Find available ports in the range 9001-9100.

The port discovery and communication process works as follows:

1. The watchdog creates pipes for each child process.
2. Child processes discover available ports during initialization.
3. Child processes report their chosen ports to the watchdog through pipes.
4. The watchdog updates environment variables with current port mappings.
5. Processes read port information from environment variables.

This approach provides several benefits:

- Prevents port conflicts when multiple instances run on the same host.
- Improves fault tolerance as failed processes can restart on different ports.
- Enables flexible scaling by adding more processes without manual port configuration.

## Building and Running

Build all components:

```bash
make
```

Start the system with the watchdog:

```bash
./watchdog
```

Test with the client:

```bash
# Send a single request
./client <client_id>
```

## Cleaning Up

```bash
make clean
```
