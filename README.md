# FMEA-MSR Event System (MSRGuard)

This repository is part of a master project thesis. The built system is a prototype for a runtime exception-handling framework for manufacturing that couples a fast C++ control path (OPC UA PLC monitoring, event bus, reactions) with a Python knowledge-graph bridge for PFMEA-MSR–driven decisions. This FMEA Exception Handling Framework is called the MSRGuard.

## Main Idea of MSRGuard (FMEA Exception-Handling-Framework)
Creating an Exception-Handling-Framework for Runtime Integration of FMEA-MSR from VDA&AIAG Standard into manufacturing control. Key Objective was to create an extensible framework that can be used by varios I4.0-Systems as shown in following Figure
![Main Idea](UML%20Diagrams/Core_Idea.png)

## Key capabilities
- **PLC Monitoring over OPC UA** – Subscribe to trigger bits, read/write variables, and call PLC methods with secure sessions.
- **Event Bus & Reactions** – Prioritized dispatch and execution of monitoring actions vs. system reactions.
- **Knowledge-Graph Bridge (Python)** – C++ ⇄ Python via a small runtime wrapper for KG querying and ingestion.
- **Failure Recording & Ingestion** – Capture snapshots, executed actions, and timing; persist/ingest for analysis.
- **Latency Tracking** – End-to-end timing to CSV for reproducible measurements.

## Repository layout
- [`/src`](src/README.md) – C++ sources (entry point, PLC monitor, event bus, reaction manager, forces, utilities).
- [`/include`](include/README.md) – Public headers (events/acks, snapshots, plans, utilities).
- [`/open62541`](open62541/README.md) – OPC UA stack as a **git submodule** (client used for secure sessions).
- [`/tools`](tools/README.md) → [`ua_test_server`](tools/ua_test_server/README.md) – Local OPC UA test server helpers.
- [`/certificates`](certificates/README.md) – Example client cert/key for Sign&Encrypt sessions.
- [`/UML Diagrams`](UML%20Diagrams/README.md) – Architecture and sequence diagrams.
- [`/.vscode`](.vscode/README.md) – VS Code settings, presets, and launch configs.
- [`/extern`](extern/README.md) – Vendored helpers (if any).

# Used Design Patterns

## Observer Pattern

The **Observer Pattern** is used to implement the event-driven core of the system.

`EventBus` acts as the *subject*, and all components that react to events (e.g. `ReactionManager`, `FailureRecorder`, `TimeBlogger`, `AckLogger`, …) implement a common observer interface and subscribe to specific `EventType`s.  

Whenever something relevant happens (a D2/D3 snapshot, a Failure Mode decision, a System Reaction result, a KG ingestion, …), an `Event` is posted to the `EventBus`. The bus then notifies all observers in a decoupled way, without the sender having to know who is listening.

This allows:
- loose coupling between PLC monitoring, knowledge graph interaction, and reaction logic  
- easy extension by just subscribing new observers to existing events  
- clear tracing of the whole failure-handling pipeline via `correlationId`

![Observer Pattern](UML%20Diagrams/Patterns/ObserverPattern.png)

---

## Factory Method and Abstract Factory

The project combines **Factory Method** and **Abstract Factory** to create the different “forces” that execute plans.

The `CommandForceFactory` encapsulates the creation of concrete `ICommandForce` implementations based on the requested `OpType` / operation context:

- `PLCCommandForce` for PLC-level operations (writes, pulses, waits, checks, resource blocking, …)  
- `KgIngestionForce` for pushing failure information into the knowledge graph  
- `WriteCSVForce` for writing time-series / analysis data to CSV

This lets the rest of the code work purely with the `ICommandForce` interface while the factory decides *which* concrete executor is appropriate for a given `Operation` or `Plan`.

![Factory Pattern1](UML%20Diagrams/Patterns/FactoryMethod_AbstractFactory1.png)

In addition, the project uses an **Abstract Factory**–like setup around `IWinnerFilter`:

- `IWinnerFilter` defines a common interface for “winner filters” that take a set of candidate Failure Modes and narrow them down.
- `MonitoringActionForce` implements `IWinnerFilter` by executing monitoring actions retrieved from the KG and checking their outputs.  
- `SystemReactionForce` implements `IWinnerFilter` by executing system reactions and verifying their feedback against expectations.

Together with the factory functions (`createWinnerFilter`, `createSystemReactionFilter`), this forms an abstract factory that can produce different “action forces” specialised on deciding which Failure Mode and reaction path is actually valid, using monitoring actions and system reactions defined in the knowledge graph.

![Factory Pattern2](UML%20Diagrams/Patterns/FactoryMethod_AbstractFactory2.png)

---

## Command Pattern

The **Command Pattern** is used to represent executable logic in a structured and replayable way.

Each **command** is represented as a `Plan`:

- `Plan` acts as a command object and groups a sequence of `Operation`s.
- Each `Operation` describes *what* should be done (e.g. `WriteBool`, `CallMethod`, `WaitMs`, `KGIngestion`, `WriteCSV`, …) but not *how* it is executed.

Concrete `ICommandForce` implementations (`PLCCommandForce`, `KgIngestionForce`, `WriteCSVForce`, …) act as the **command executors**: they interpret the `Plan` and perform the corresponding actions against the PLC, the knowledge graph, or the filesystem.

This separation allows:

- building and transforming plans from JSON / KG results (see `PlanJsonUtils`)  
- logging and replaying command sequences  
- swapping the executor (e.g. PLC vs. mock vs. test harness) without changing the plan structure

![Command Pattern](UML%20Diagrams/Patterns/CommandPattern.png)


## Prerequisites to get the MSRGuard running
- **CMake** ≥ 3.25 and a **C++20** compiler (GCC/Clang/MSVC).
- **Python** 3.11 or 3.12 available at runtime for the KG bridge (ensure your Python site-packages are discoverable).
- **OpenSSL** (for OPC UA Sign&Encrypt).
- **Git** with submodule support.

## Quick start

```bash
# 1) Clone with submodules
git clone --recurse-submodules https://github.com/verkal1999/FMEA-MSR-EventSystem.git
cd FMEA-MSR-EventSystem

# 2) Configure (choose your build type)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 3) Build
cmake --build build -j
``````

If you didn’t pass --recurse-submodules, run:
git submodule update --init --recursive.

## Run
1. Set your OPC UA endpoint and security (cert/key paths).
2. Ensure the Python module path for your KG code is on sys.path.
3. Start the executable from build/.
4. On trigger events, the system snapshots state, posts events, optionally queries/ingests KG data, and writes timing CSVs in logs/time/.

## Configuration tips
Security: Use Sign&Encrypt (e.g., Basic256Sha256) with your own certificate and private key.
OPC UA nodes: Adjust the trigger and method NodeIds to match your PLC/test server.
Logging: Timing CSVs are written under build/Debug/logs/time/. Keep long-running tests in a separate folder per run.

