# AircraftTelemetry

A multi-threaded C++ client/server telemetry processing system built in Visual Studio. The project simulates real-time aircraft telemetry streaming from onboard clients to a ground-based server that calculates **live and final average fuel consumption statistics** for each aircraft.

##  Features

* Multi-threaded **server supports multiple concurrent aircraft clients**
* Each client reads **real telemetry flight data files**
* Unique **aircraft ID assigned per client session**
* Real-time processing of:

  * elapsed flight time
  * fuel remaining
  * average fuel burn (gal/hr)
* Final flight summary generated on disconnect
* CSV export of completed flight statistics
* Updated **telemetry-based duration calculation** for accurate final flight duration

##  Project Structure

```text
AircraftTelemetry/
├── Client/
├── Server/
├── shared/
├── TelemetryData/
└── x64/
```

##  Requirements

* Visual Studio 2022
* Desktop Development with C++
* Windows SDK
* vcpkg pthreads package

Install pthreads:

```powershell
cd C:\vcpkg
.\vcpkg install pthreads:x64-windows
```

##  How to Run

### Start Server

Set Server command arguments:

```text
5000
```

### Start Client

Set Client command arguments:

```text
127.0.0.1 5000 "C:\path\to\TelemetryData\Data Files\katl-kefd-B737-700.txt"
```

### Run both in Visual Studio

Use **Multiple Startup Projects**:

* Server = Start
* Client = Start

Then press:

```text
Ctrl + F5
```

##  Example Output

```text
Aircraft ID    : 1775116880
Avg Fuel Burn  : 648.3 gal/hr
Duration       : 10508 sec
Packets recvd  : 8565
```

##  Recent Fixes

* Fixed final flight duration to use **telemetry timestamps instead of wall-clock time**
* Improved final statistics consistency for Part A and performance testing
* Better demo readiness for load/endurance testing

##  Contributors

* Faisal Ahmed — Client module updates, telemetry statistics fix
* Priyansh - Server update
* Team members — server core, threading, telemetry parsing, performance testing

##  Course Context

Developed for **CSCN73060 – Client/Server Project**.
Focus areas:

* distributed systems
* socket programming
* multi-threading
* performance profiling
* load, spike, and crash testing
