# tools/ua_test_server/

A small OPC UA test server to exercise the client:

## Why
- Develop and debug subscriptions, reads/writes, and method calls without touching a real PLC.

## How
1. Build or launch the test server (follow the scripts/config in this folder).
2. Note the **endpoint URL** and **security mode** it exposes.
3. Point the application’s PLC monitor to this endpoint.
4. If security is enabled, trust the server certificate or adjust policy as needed.

## Tips
- Start simple (no encryption) to validate NodeIds and basic flows, then enable Sign&Encrypt.
- Mirror the trigger/method NodeIds you plan to use on the PLC to keep tests realistic.
