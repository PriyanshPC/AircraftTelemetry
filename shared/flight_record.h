#ifndef FLIGHT_RECORD_H
#define FLIGHT_RECORD_H

#include <stdint.h>
#include <time.h>

/*
 * FlightRecord
 * ------------
 * Per-aircraft state maintained in server memory during an active flight.
 * Stored in a shared std::map keyed by aircraft_id.
 * ALL access to this map must be protected by records_mutex (server_core.cpp).
 *
 * !! DO NOT MODIFY THIS STRUCT AFTER DISTRIBUTING TO THE TEAM !!
 */
typedef struct {
    uint32_t  aircraft_id;      /* Unique aircraft identifier                        */
    float     initial_fuel;     /* Fuel reading captured from the very first packet   */
    float     latest_fuel;      /* Most recently received fuel value                 */
    float     latest_elapsed;   /* Most recently received elapsed time (seconds)     */
    float     avg_consumption;  /* Running average fuel consumption (gallons/hour)   */
    int       packet_count;     /* Total number of packets received this flight      */
    time_t    session_start;    /* Wall-clock time when the first packet arrived      */
} FlightRecord;

#endif /* FLIGHT_RECORD_H */
```

-- -

## Step 6: Add Source Files to Each Project

### Server project files

Right - click * *Server * *in Solution Explorer → * *Add → New Item * *

Add these files one by one(select * *C++ File(.cpp) * *or **Header File(.h) * *as appropriate) :

    | File | Type | Owner |
    |------ | ------ | ------ - |
    | `server_core.h` | Header File(.h) | Priyansh |
    | `server_core.cpp` | C++ File(.cpp) | Priyansh |
    | `data_processing.h` | Header File(.h) | Member 3 |
    | `data_processing.cpp` | C++ File(.cpp) | Member 3 |

    ### Client project files

    Right - click * *Client * *in Solution Explorer → * *Add → New Item * *

    | File | Type | Owner |
    |------ | ------ | ------ - |
    | `client.cpp` | C++ File(.cpp) | Member 2 |

    -- -

    ## Step 7: Link Shared Headers Into Both Projects

    The shared headers live in `shared/` but both projects need to see them.Do this for** both Server and Client** :

1. Right - click * *Server * *→ * *Add → Existing Item * *
2. Navigate to `AircraftTelemetry/shared / `
3. Select * *both * *`telemetry_packet.h` and `flight_record.h`
4. Click * *Add * *

Repeat for** Client** (Client only needs `telemetry_packet.h` but add both anyway for visibility).

You'll now see them appear under each project in Solution Explorer. These are **links** not copies — one file, referenced by both projects.

-- -

## Step 8: Configure Include Paths

Both projects need to know where `shared/` lives so `#include "telemetry_packet.h"` works without any path prefix.

Do this for** both Server and Client** :

1. Right - click project → * *Properties * *
2. At the top — set Configuration : **All Configurations**, Platform : **All Platforms**
3. Go to * *C / C++ → General → Additional Include Directories * *
4. Click the dropdown → * *Edit * *
5. Click the yellow folder icon → type :
```
$(SolutionDir)shared\