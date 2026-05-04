# AI Agent Instructions - ESP IoT Log

This project provides a lightweight, multicast-based logging library for IoT devices.

## 1. Core Architectural Mandate: Multicast Only
The primary purpose and intent of using multicast for logging is:
- **Zero Configuration:** The IoT device should not be configured with a specific logging destination IP. It simply joins a well-known multicast group.
- **Multiple Recipients:** Multicast allows any number of LAN recipients to receive the logs simultaneously without increasing the device's transmit overhead.
- **Discovery:** The beacon-based discovery mechanism allows the device to stay silent until a listener is active on the network.

**STRICT RULE:** Never suggest or implement a permanent switch to unicast logging. Even if multicast routing (BBR/MLD) is failing in a specific environment, the solution should focus on fixing the multicast plumbing or providing temporary diagnostic workarounds, not changing the fundamental architecture of the library.
